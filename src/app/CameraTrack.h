#pragma once

#include "mmd/MmdModel.h"

#include <mmd/vmd.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace uka3d {

// Camera moves that play together with a motion.
//
// A track is a list of keys relative to the default camera (the one fitted to
// the model, including the user's wheel zoom and the seat/walk adjustments):
//   focus  model-space point that is brought to where the default camera looks
//          (a fixed point, or a bone followed in its current pose plus an offset;
//          omitted = the default look-at point)
//   zoom   how much larger the focus appears (the camera distance is divided by it)
//   yaw    orbit around the focus in degrees (+ = the camera moves to the
//          character's left, i.e. it turns toward the viewer's right)
//   pitch  orbit in degrees (+ = the camera rises and looks down)
//   ease   how the key is reached from the previous one: linear / smooth / in / out
//
// Only the camera moves; the model, its physics and the window are untouched.
// File format (UTF-8 JSON, usually next to the VMD as <name>.camera.json):
//   {"version": 1, "keys": [{"frame": 0}, {"frame": 30, "focusBone": "頭", "zoom": 1.8,
//                                          "yaw": -10, "ease": "smooth"}, {"frame": 90}]}
// Frames are VMD frames (30 per second).
//
// An MMD camera motion (a VMD with camera keys, usually <name>.camera.vmd) works
// as well. It is applied relative to MMD's initial camera (look-at (0, 10, 0),
// distance 45, 30 degrees), so a camera made in MMD frames the character the
// same way on a standard-sized model while still fitting this window, the
// wheel zoom and the seat/walk adjustments. MMD's interpolation curves are
// used as they are; the orthographic setting is ignored (always perspective).
enum class CameraEase : std::uint8_t { linear, smooth, easeIn, easeOut };

struct CameraKey {
    double frame{0.0};
    std::optional<mmd::Float3> focus; // fixed model-space point
    std::string focusBone;            // or a bone (with focusOffset), followed as it moves
    mmd::Float3 focusOffset{0.0F, 0.0F, 0.0F};
    float zoom{1.0F};
    float yawDegrees{0.0F};
    float pitchDegrees{0.0F};
    CameraEase ease{CameraEase::smooth};
};

// The camera adjustment at one moment. focus is absolute (model space).
struct CameraView {
    mmd::Float3 focus{0.0F, 0.0F, 0.0F};
    float zoom{1.0F};
    float yawDegrees{0.0F};
    float pitchDegrees{0.0F};
    float rollDegrees{0.0F};
    float fovDegrees{0.0F}; // 0 = keep the default field of view
};

// MMD's initial camera, the reference for camera VMDs.
inline constexpr float kMmdDefaultDistance = 45.0F;
inline constexpr float kMmdDefaultTargetY = 10.0F;
inline constexpr float kMmdDefaultFov = 30.0F;
// One evaluated MMD camera state as a view relative to the default camera.
CameraView mmdCameraView(const mmd::VmdCameraState &state, const mmd::Float3 &defaultFocus);

class CameraTrack {
  public:
    // Throws std::runtime_error with a Japanese message on invalid input.
    static CameraTrack fromJson(const nlohmann::json &json);
    // .camera.json, or a VMD with camera keys (.vmd).
    static CameraTrack load(const std::filesystem::path &path);
    static CameraTrack fromVmd(mmd::VmdMotion motion);
    // Sidecar files next to a motion, in the order they are looked for:
    // <motion stem>.camera.vmd, then <motion stem>.camera.json.
    static std::vector<std::filesystem::path> sidecarPaths(const std::filesystem::path &motionPath);

    [[nodiscard]] const std::vector<CameraKey> &keys() const noexcept {
        return keys_;
    }
    [[nodiscard]] double lastFrame() const noexcept {
        if (vmd_)
            return static_cast<double>(vmd_->lastFrame);
        return keys_.empty() ? 0.0 : keys_.back().frame;
    }
    [[nodiscard]] bool isVmd() const noexcept {
        return vmd_.has_value();
    }
    [[nodiscard]] std::size_t keyCount() const noexcept {
        return vmd_ ? vmd_->cameras.size() : keys_.size();
    }

    // Resolves a key's focus to a model-space point; nullopt = the default look-at point.
    using FocusResolver = std::function<std::optional<mmd::Float3>(const CameraKey &)>;
    // The view at a motion frame (held at the first/last key outside the keyed range).
    [[nodiscard]] CameraView sample(double frame, const mmd::Float3 &defaultFocus,
                                    const FocusResolver &resolve) const;

  private:
    std::vector<CameraKey> keys_;
    std::optional<mmd::VmdMotion> vmd_;
};

// The neutral view (the default camera unchanged).
CameraView neutralCameraView(const mmd::Float3 &defaultFocus);
// Blends two views (t = 0 -> a, 1 -> b); zoom is blended in log space.
CameraView blendCameraViews(const CameraView &a, const CameraView &b, float t);
float applyCameraEase(CameraEase ease, float t);

} // namespace uka3d
