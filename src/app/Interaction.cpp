#include "app/Interaction.h"

#include "ipc/Protocol.h"
#include "platform/Log.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace uka3d {
namespace {

template <typename T>
T bounded(const ipc::Json &value, T minimum, T maximum) {
    if (!value.is_number() || (std::is_integral_v<T> && !value.is_number_integer()))
        throw std::runtime_error("数値の型が不正です");
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < minimum || number > maximum)
        throw std::runtime_error("数値が指定範囲外です");
    return static_cast<T>(number);
}

bool validRegionName(const std::string &name) {
    return !name.empty() && name.size() <= 96 && std::none_of(name.begin(), name.end(), [](unsigned char ch) {
        return ch < 32 || ch == 127;
    });
}

} // namespace

InteractionProfile InteractionProfile::load(const std::filesystem::path &path) {
    constexpr std::uintmax_t maxProfileBytes = 128 * 1024;
    if (std::filesystem::file_size(path) > maxProfileBytes)
        throw std::runtime_error("当たり判定設定は 128 KiB までです");
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("当たり判定設定を開けません");
    const auto json = ipc::Json::parse(file, nullptr, true, true);
    InteractionProfile profile;
    profile.mouseMove = json.value("mouseMove", profile.mouseMove);
    const auto pet = json.value("pet", ipc::Json::object());
    profile.pet.distance = bounded<float>(pet.value("distance", ipc::Json(profile.pet.distance)), 1.0F, 2000.0F);
    profile.pet.minSegment = bounded<float>(pet.value("minSegment", ipc::Json(profile.pet.minSegment)), 1.0F, 100.0F);
    profile.pet.windowMs = bounded<int>(pet.value("windowMs", ipc::Json(profile.pet.windowMs)), 100, 5000);
    profile.pet.turns = bounded<int>(pet.value("turns", ipc::Json(profile.pet.turns)), 1, 10);
    profile.pet.cooldownMs = bounded<int>(pet.value("cooldownMs", ipc::Json(profile.pet.cooldownMs)), 0, 30000);
    const auto &regions = json.at("regions");
    if (!regions.is_array() || regions.size() > 128)
        throw std::runtime_error("regions は最大 128 個の配列です");
    for (const auto &entry : regions) {
        Sphere sphere;
        sphere.name = entry.at("name").get<std::string>();
        sphere.bone = entry.at("bone").get<std::string>();
        if (!validRegionName(sphere.name) || sphere.bone.empty())
            throw std::runtime_error("当たり判定名またはボーン名が不正です");
        sphere.radius = bounded<float>(entry.at("radius"), 0.001F, 10000.0F);
        sphere.priority = bounded<int>(entry.value("priority", ipc::Json(0)), -1000, 1000);
        if (const auto offset = entry.find("offset"); offset != entry.end()) {
            if (!offset->is_array() || offset->size() != 3)
                throw std::runtime_error("offset には X, Y, Z の 3 要素が必要です");
            for (std::size_t axis = 0; axis < sphere.offset.size(); ++axis)
                sphere.offset[axis] = bounded<float>((*offset)[axis], -10000.0F, 10000.0F);
        }
        profile.spheres.push_back(std::move(sphere));
    }
    return profile;
}

void HitRegions::bind(const InteractionProfile &profile, const MmdModel &model) {
    spheres_.clear();
    for (const auto &sphere : profile.spheres) {
        if (const auto bone = model.findBone(sphere.bone))
            spheres_.push_back({sphere, *bone});
        else
            log::warn("当たり判定 {}: ボーン {} がモデルにありません", sphere.name, sphere.bone);
    }
}

std::string_view HitRegions::pick(const CameraRay &ray, const MmdModel &model) const {
    using namespace DirectX;
    const auto origin = XMLoadFloat3(&ray.origin);
    const auto direction = XMLoadFloat3(&ray.direction);
    int priority = std::numeric_limits<int>::min();
    float nearest = ray.length;
    std::string_view result;
    for (const auto &[sphere, bone] : spheres_) {
        if (sphere.priority < priority)
            continue;
        const auto center = model.transformBoneOffset(bone, sphere.offset);
        const auto delta = XMVectorSubtract(origin, XMVectorSet(center[0], center[1], center[2], 0.0F));
        const float along = XMVectorGetX(XMVector3Dot(delta, direction));
        const float c = XMVectorGetX(XMVector3LengthSq(delta)) - sphere.radius * sphere.radius;
        const float discriminant = along * along - c;
        if (discriminant < 0)
            continue;
        const float root = std::sqrt(discriminant);
        const float entry = std::max(0.0F, -along - root);
        if (-along + root < 0 || entry >= ray.length || (sphere.priority == priority && entry >= nearest))
            continue;
        result = sphere.name;
        priority = sphere.priority;
        nearest = entry;
    }
    return result;
}

InteractionInput::InteractionInput(PetSettings settings, bool mouseMove, std::uint64_t doubleClickMs, Emit emit)
    : settings_(settings), mouseMove_(mouseMove), doubleClickMs_(doubleClickMs), emit_(std::move(emit)) {}

void InteractionInput::flushClick() {
    if (!pending_)
        return;
    emit_(MouseEventKind::click, pending_->sample);
    pending_.reset();
}

void InteractionInput::press(PointerSample sample, std::uint64_t now, bool osDoubleClick) {
    leave();
    secondClick_ = osDoubleClick && pending_ && now <= pending_->deadline && sample.overModel &&
                   sample.region == pending_->sample.region;
    if (!secondClick_)
        flushClick();
    pressedAt_ = now;
    pressed_ = std::move(sample);
}

void InteractionInput::release(const PointerSample &sample, std::uint64_t now, bool held) {
    if (pressed_ && pressed_->overModel && sample.overModel && pressed_->region == sample.region && !held) {
        if (secondClick_) {
            pending_.reset();
            emit_(MouseEventKind::doubleClick, sample);
        } else {
            pending_ = PendingClick{sample, std::max(now, pressedAt_ + doubleClickMs_)};
        }
    }
    cancelPress();
}

void InteractionInput::cancelPress() {
    pressed_.reset();
    secondClick_ = false;
    leave();
}

void InteractionInput::startDrag(std::uint64_t now) {
    if (pressed_) {
        dragging_ = true;
        if (pressed_->overModel)
            emit_(MouseEventKind::dragStart, *pressed_);
    }
    secondClick_ = false;
    tick(now);
}

void InteractionInput::endDrag(PointerSample sample) {
    if (dragging_ && pressed_ && pressed_->overModel) {
        sample.region = pressed_->region;
        emit_(MouseEventKind::dragEnd, sample);
    }
    dragging_ = false;
    cancelPress();
}

void InteractionInput::leave() {
    lastMove_.reset();
    stroke_.reset();
}

void InteractionInput::resetStroke(PointerSample sample, std::uint64_t now) {
    stroke_ = std::move(sample);
    strokeStart_ = now;
    lastDx_ = lastDy_ = distance_ = 0.0F;
    turns_ = 0;
}

void InteractionInput::move(PointerSample sample, std::uint64_t now, bool buttonsDown) {
    if (!sample.overModel || buttonsDown || pressed_ || dragging_) {
        leave();
        return;
    }
    const bool moved = !lastMove_ || sample.screenX != lastMove_->screenX || sample.screenY != lastMove_->screenY;
    if (!moved && sample.region == lastMove_->region)
        return;
    lastMove_ = sample;
    if (moved && mouseMove_ && now >= nextMove_) {
        emit_(MouseEventKind::move, sample);
        nextMove_ = now + 67;
    }
    if (sample.region.empty() || now < nextPet_) {
        stroke_.reset();
        return;
    }
    if (!stroke_ || stroke_->region != sample.region || stroke_->dpiScale != sample.dpiScale ||
        now - strokeStart_ > static_cast<std::uint64_t>(settings_.windowMs)) {
        resetStroke(std::move(sample), now);
        return;
    }
    const float dx = (sample.screenX - stroke_->screenX) / sample.dpiScale;
    const float dy = (sample.screenY - stroke_->screenY) / sample.dpiScale;
    const float length = std::hypot(dx, dy);
    if (length < settings_.minSegment)
        return;
    const float nx = dx / length;
    const float ny = dy / length;
    if (nx * lastDx_ + ny * lastDy_ <= -0.5F)
        ++turns_;
    lastDx_ = nx;
    lastDy_ = ny;
    distance_ += length;
    if (distance_ >= settings_.distance && turns_ >= settings_.turns) {
        emit_(MouseEventKind::pet, sample);
        nextPet_ = now + static_cast<std::uint64_t>(settings_.cooldownMs);
        stroke_.reset();
        return;
    }
    stroke_ = std::move(sample);
}

void InteractionInput::tick(std::uint64_t now) {
    if (pending_ && !secondClick_ && now >= pending_->deadline)
        flushClick();
}

} // namespace uka3d
