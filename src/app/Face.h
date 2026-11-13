#pragma once

#include "mmd/MmdModel.h"

#include <algorithm>
#include <cstdint>
#include <random>

namespace uka3d {

// Automatic blinking and gaze following. Pure logic (no Win32/D3D) so it can be
// unit-tested; App feeds it time, the cursor ray and the head pose, and hands
// the results to MmdModel as a morph weight and bone offsets that libmmd
// applies before IK and physics (hair follows the turned head).

struct BlinkSettings {
    bool enabled{true};
    float minInterval{2.0F}; // seconds between blinks (random in [min, max])
    float maxInterval{6.0F};
    float closeSeconds{0.06F};
    float holdSeconds{0.04F};
    float openSeconds{0.11F};
    float doubleChance{0.15F}; // a second blink right after the first
};

class BlinkScheduler {
  public:
    explicit BlinkScheduler(const BlinkSettings &settings = {}, std::uint32_t seed = 0x6c61696eU);
    // Advances by dt seconds and returns the eyelid weight (0 open .. 1 closed).
    float update(double dt);
    // Postpones the next blink (the eyes are already closed by a motion or an expression).
    void hold();
    void setSettings(const BlinkSettings &settings);
    [[nodiscard]] const BlinkSettings &settings() const noexcept {
        return settings_;
    }
    [[nodiscard]] bool blinking() const noexcept {
        return phase_ != Phase::open;
    }
    [[nodiscard]] double secondsToNext() const noexcept {
        return phase_ == Phase::open ? std::max(0.0, wait_ - t_) : 0.0;
    }

  private:
    enum class Phase : std::uint8_t { open, closing, closed, opening };
    void scheduleNext(bool allowDouble);

    BlinkSettings settings_;
    std::mt19937 random_;
    Phase phase_{Phase::open};
    double wait_{0.0};
    double t_{0.0};
    bool doublePending_{false};
};

struct GazeSettings {
    bool enabled{true};
    float radius{0.0F};        // reaction distance from the head in 96-dpi pixels; 0 = automatic
    float maxYaw{40.0F};       // degrees, total over neck + head + eyes
    float maxPitch{25.0F};
    float followSeconds{0.18F}; // smoothing time constant while following
    float releaseSeconds{0.6F}; // fade back when the cursor leaves
    float neckShare{0.30F};
    float headShare{0.45F};
    float eyeShare{0.25F};
};

namespace facemath {
using Quat = mmd::Float4; // x, y, z, w
inline constexpr Quat kIdentity{0.0F, 0.0F, 0.0F, 1.0F};
Quat multiply(const Quat &a, const Quat &b);
Quat conjugate(const Quat &q);
mmd::Float3 rotate(const Quat &q, const mmd::Float3 &v);
Quat between(const mmd::Float3 &from, const mmd::Float3 &to); // shortest arc, unit vectors
Quat slerp(const Quat &a, const Quat &b, float t);
float angleDegrees(const Quat &q);
} // namespace facemath

// The rotation, in the head's own frame, that turns the head's forward (-Z)
// toward `toward` (model space), limited to maxYaw / maxPitch.
//   headRotation  the head's current global rotation without any gaze offset
facemath::Quat gazeRotation(const facemath::Quat &headRotation, const mmd::Float3 &toward, float maxYawDegrees,
                            float maxPitchDegrees);

// Where the character should look for a cursor ray: a point on the ray between
// the camera and the eyes, so a cursor over the face means "look at the viewer".
mmd::Float3 gazeTarget(const mmd::Float3 &cameraEye, const mmd::Float3 &rayDirection, const mmd::Float3 &eyes);

// Smoothly follows the wanted gaze and fades in and out.
class GazeFollower {
  public:
    explicit GazeFollower(const GazeSettings &settings = {}) : settings_(settings) {}
    // active: the cursor is near; wanted: gazeRotation() for it. Returns true if the output changed.
    bool update(double dt, bool active, const facemath::Quat &wanted);
    void reset();
    void setSettings(const GazeSettings &settings) {
        settings_ = settings;
    }
    [[nodiscard]] const GazeSettings &settings() const noexcept {
        return settings_;
    }
    [[nodiscard]] float weight() const noexcept {
        return weight_;
    }
    // The share of the current gaze for one bone (neck/head/eyes share).
    [[nodiscard]] facemath::Quat part(float share) const;
    // The part of the gaze that already turns the head (neck + head shares).
    [[nodiscard]] facemath::Quat headPart() const {
        return part(settings_.neckShare + settings_.headShare);
    }

  private:
    GazeSettings settings_;
    facemath::Quat current_{facemath::kIdentity};
    float weight_{0.0F};
};

} // namespace uka3d
