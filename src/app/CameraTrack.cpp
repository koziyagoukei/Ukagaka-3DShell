#include "app/CameraTrack.h"

#include "platform/Utf.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace uka3d {

namespace {

constexpr float kMinZoom = 0.2F;
constexpr float kMaxZoom = 8.0F;

float number(const nlohmann::json &key, const char *name, float fallback, std::size_t index) {
    const auto it = key.find(name);
    if (it == key.end() || it->is_null())
        return fallback;
    if (!it->is_number())
        throw std::runtime_error("カメラのキー " + std::to_string(index) + " の " + name + " は数値で指定してください");
    const double value = it->get<double>();
    if (!std::isfinite(value))
        throw std::runtime_error("カメラのキー " + std::to_string(index) + " の " + name + " が不正です");
    return static_cast<float>(value);
}

mmd::Float3 vector3(const nlohmann::json &value, const char *name, std::size_t index) {
    if (!value.is_array() || value.size() != 3 || !std::ranges::all_of(value, [](const auto &v) { return v.is_number(); }))
        throw std::runtime_error("カメラのキー " + std::to_string(index) + " の " + name + " は [x, y, z] で指定してください");
    return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
}

CameraEase parseEase(const nlohmann::json &key, std::size_t index) {
    const auto it = key.find("ease");
    if (it == key.end() || it->is_null())
        return CameraEase::smooth;
    const std::string text = it->is_string() ? it->get<std::string>() : std::string();
    if (text == "linear")
        return CameraEase::linear;
    if (text == "smooth")
        return CameraEase::smooth;
    if (text == "in")
        return CameraEase::easeIn;
    if (text == "out")
        return CameraEase::easeOut;
    throw std::runtime_error("カメラのキー " + std::to_string(index) + " の ease は linear / smooth / in / out のどれかです");
}

mmd::Float3 lerp3(const mmd::Float3 &a, const mmd::Float3 &b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

float lerpZoom(float a, float b, float t) {
    return std::exp(std::log(a) + (std::log(b) - std::log(a)) * t);
}

} // namespace

float applyCameraEase(CameraEase ease, float t) {
    t = std::clamp(t, 0.0F, 1.0F);
    switch (ease) {
    case CameraEase::linear:
        return t;
    case CameraEase::easeIn:
        return t * t;
    case CameraEase::easeOut:
        return 1.0F - (1.0F - t) * (1.0F - t);
    case CameraEase::smooth:
    default:
        return t * t * (3.0F - 2.0F * t);
    }
}

CameraTrack CameraTrack::fromJson(const nlohmann::json &json) {
    if (!json.is_object())
        throw std::runtime_error("カメラトラックは JSON オブジェクトで書いてください");
    if (const auto v = json.find("version"); v != json.end() && !(v->is_number_integer() && v->get<int>() == 1))
        throw std::runtime_error("対応していないカメラトラックの version です（1 のみ）");
    const auto keys = json.find("keys");
    if (keys == json.end() || !keys->is_array() || keys->empty())
        throw std::runtime_error("カメラトラックに keys（1 つ以上のキー）がありません");
    CameraTrack track;
    for (std::size_t i = 0; i < keys->size(); ++i) {
        const auto &k = (*keys)[i];
        if (!k.is_object())
            throw std::runtime_error("カメラのキー " + std::to_string(i) + " はオブジェクトで指定してください");
        CameraKey key;
        key.frame = number(k, "frame", -1.0F, i);
        if (key.frame < 0.0)
            throw std::runtime_error("カメラのキー " + std::to_string(i) + " に 0 以上の frame が必要です");
        if (const auto f = k.find("focus"); f != k.end() && !f->is_null())
            key.focus = vector3(*f, "focus", i);
        if (const auto b = k.find("focusBone"); b != k.end() && !b->is_null()) {
            if (!b->is_string() || b->get<std::string>().empty())
                throw std::runtime_error("カメラのキー " + std::to_string(i) + " の focusBone はボーン名で指定してください");
            if (key.focus)
                throw std::runtime_error("カメラのキー " + std::to_string(i) + " に focus と focusBone の両方があります");
            key.focusBone = b->get<std::string>();
        }
        if (const auto o = k.find("focusOffset"); o != k.end() && !o->is_null())
            key.focusOffset = vector3(*o, "focusOffset", i);
        key.zoom = std::clamp(number(k, "zoom", 1.0F, i), kMinZoom, kMaxZoom);
        key.yawDegrees = number(k, "yaw", 0.0F, i);
        key.pitchDegrees = std::clamp(number(k, "pitch", 0.0F, i), -80.0F, 80.0F);
        key.ease = parseEase(k, i);
        track.keys_.push_back(std::move(key));
    }
    std::ranges::stable_sort(track.keys_, {}, &CameraKey::frame);
    return track;
}

CameraTrack CameraTrack::fromVmd(mmd::VmdMotion motion) {
    if (motion.cameras.empty())
        throw std::runtime_error("VMD にカメラのキーがありません（MMD でカメラモーションとして保存したものを使ってください）");
    CameraTrack track;
    track.vmd_ = std::move(motion);
    return track;
}

CameraTrack CameraTrack::load(const std::filesystem::path &path) {
    if (path.extension() == L".vmd" || path.extension() == L".VMD") {
        try {
            return fromVmd(mmd::loadVmd(path));
        } catch (const std::exception &e) {
            throw std::runtime_error("カメラモーションを読み込めません (" + pathToUtf8(path) + "): " + e.what());
        }
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("カメラトラックを開けません: " + pathToUtf8(path));
    std::stringstream buffer;
    buffer << in.rdbuf();
    nlohmann::json json;
    try {
        // comments allowed, like ukagaka3d.json
        json = nlohmann::json::parse(buffer.str(), nullptr, true, true);
    } catch (const nlohmann::json::exception &e) {
        throw std::runtime_error("カメラトラックの JSON が不正です (" + pathToUtf8(path) + "): " + e.what());
    }
    try {
        return fromJson(json);
    } catch (const std::runtime_error &e) {
        throw std::runtime_error(std::string(e.what()) + " (" + pathToUtf8(path) + ")");
    }
}

std::vector<std::filesystem::path> CameraTrack::sidecarPaths(const std::filesystem::path &motionPath) {
    std::filesystem::path vmd = motionPath;
    vmd.replace_extension(L".camera.vmd");
    std::filesystem::path json = motionPath;
    json.replace_extension(L".camera.json");
    return {vmd, json};
}

CameraView neutralCameraView(const mmd::Float3 &defaultFocus) {
    return CameraView{defaultFocus, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F};
}

CameraView blendCameraViews(const CameraView &a, const CameraView &b, float t) {
    t = std::clamp(t, 0.0F, 1.0F);
    // An unset field of view (0) is the default camera's (30 degrees, as in MMD).
    const bool anyFov = a.fovDegrees > 0.0F || b.fovDegrees > 0.0F;
    const float fovA = a.fovDegrees > 0.0F ? a.fovDegrees : kMmdDefaultFov;
    const float fovB = b.fovDegrees > 0.0F ? b.fovDegrees : kMmdDefaultFov;
    return CameraView{lerp3(a.focus, b.focus, t),
                      lerpZoom(a.zoom, b.zoom, t),
                      a.yawDegrees + (b.yawDegrees - a.yawDegrees) * t,
                      a.pitchDegrees + (b.pitchDegrees - a.pitchDegrees) * t,
                      a.rollDegrees + (b.rollDegrees - a.rollDegrees) * t,
                      anyFov ? fovA + (fovB - fovA) * t : 0.0F};
}

// MMD camera conventions (left-handed, the model faces -Z): the camera sits at
// distance (negative = in front) from the look-at point, rotated by rotation
// (radians). rotation.x < 0 raises the camera so it looks down; rotation.y > 0
// moves it toward +X (the viewer's right). This camera's pitch/yaw are the
// opposite way round, hence the sign flips.
CameraView mmdCameraView(const mmd::VmdCameraState &state, const mmd::Float3 &defaultFocus) {
    constexpr float kRadToDeg = 57.29577951F;
    CameraView view;
    view.focus = {defaultFocus[0] + state.position[0], defaultFocus[1] + (state.position[1] - kMmdDefaultTargetY),
                  defaultFocus[2] + state.position[2]};
    const float distance = std::max(0.5F, std::abs(state.distance));
    view.zoom = std::clamp(kMmdDefaultDistance / distance, 0.02F, 50.0F);
    view.pitchDegrees = -state.rotation[0] * kRadToDeg;
    view.yawDegrees = -state.rotation[1] * kRadToDeg;
    view.rollDegrees = state.rotation[2] * kRadToDeg;
    view.fovDegrees = std::clamp(state.viewAngle, 1.0F, 125.0F);
    return view;
}

CameraView CameraTrack::sample(double frame, const mmd::Float3 &defaultFocus, const FocusResolver &resolve) const {
    if (vmd_)
        return mmdCameraView(mmd::evaluateCamera(*vmd_, static_cast<float>(std::max(0.0, frame))), defaultFocus);
    if (keys_.empty())
        return neutralCameraView(defaultFocus);
    const auto view = [&](const CameraKey &key) {
        const auto focus = resolve ? resolve(key) : key.focus;
        return CameraView{focus.value_or(defaultFocus), key.zoom, key.yawDegrees, key.pitchDegrees, 0.0F, 0.0F};
    };
    if (frame <= keys_.front().frame)
        return view(keys_.front());
    for (std::size_t i = 1; i < keys_.size(); ++i) {
        const CameraKey &b = keys_[i];
        if (frame <= b.frame) {
            const CameraKey &a = keys_[i - 1];
            const double span = b.frame - a.frame;
            const float t = span <= 0.0 ? 1.0F : static_cast<float>((frame - a.frame) / span);
            return blendCameraViews(view(a), view(b), applyCameraEase(b.ease, t));
        }
    }
    return view(keys_.back());
}

} // namespace uka3d
