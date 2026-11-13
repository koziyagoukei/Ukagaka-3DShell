#include "app/Playback.h"

#include <algorithm>
#include <cmath>

namespace uka3d {

Playback::Playback(const Config &config) : config_(config) {}

void Playback::setPaused(bool paused) {
    // update() ignores elapsed time while paused, so wall-clock time spent
    // paused never reaches the simulation; the sub-tick remainder is kept so
    // resuming continues exactly where it stopped.
    paused_ = paused;
}

void Playback::rewind() {
    frame_ = 0.0;
    accumulator_ = 0.0;
    pendingSeek_ = true;
}

void Playback::restart(const Config &config) {
    const bool fixed = config_.fixedStep;
    config_ = config;
    config_.fixedStep = fixed; // physics mode is owned by setFixedStep()
    frame_ = 0.0;
    paused_ = false;
    if (!pendingSeek_)
        pendingRestart_ = true;
}

void Playback::setFixedStep(bool fixedStep) {
    if (config_.fixedStep == fixedStep)
        return;
    config_.fixedStep = fixedStep;
    accumulator_ = 0.0;
    pendingSeek_ = true;
}

float Playback::interpolationAlpha() const noexcept {
    if (!config_.fixedStep || paused_)
        return 1.0F;
    return static_cast<float>(std::clamp(accumulator_ / kTickSeconds, 0.0, 1.0));
}

Playback::Step Playback::update(double realSeconds) {
    if (!std::isfinite(realSeconds) || realSeconds < 0.0)
        realSeconds = 0.0;

    if (pendingSeek_) {
        pendingSeek_ = false;
        accumulator_ = 0.0;
        return {Kind::seek, static_cast<float>(frame_), 0.0F, 0};
    }
    if (paused_)
        return {};

    if (config_.fixedStep) {
        accumulator_ += realSeconds;
        int ticks = static_cast<int>(std::floor(accumulator_ / kTickSeconds));
        if (ticks > kMaxTicksPerUpdate) {
            // Drop the excess instead of simulating a burst of catch-up steps.
            dropped_ += (ticks - kMaxTicksPerUpdate) * kTickSeconds;
            ticks = kMaxTicksPerUpdate;
            accumulator_ = std::fmod(accumulator_, kTickSeconds) + ticks * kTickSeconds;
        }
        if (ticks <= 0)
            return {};
        accumulator_ -= ticks * kTickSeconds;
        totalTicks_ += static_cast<std::uint64_t>(ticks);
        return advanceMotion(ticks * kTickSeconds, ticks);
    }

    // Variable step (physics off): only motion playback changes the pose.
    if (!config_.hasMotion && !pendingRestart_)
        return {};
    if (realSeconds > kMaxVariableStep) {
        dropped_ += realSeconds - kMaxVariableStep;
        realSeconds = kMaxVariableStep;
    }
    if (realSeconds <= 0.0)
        return {};
    return advanceMotion(realSeconds, 0);
}

Playback::Step Playback::advanceMotion(double seconds, int ticks) {
    Kind kind = Kind::advance;
    if (pendingRestart_) {
        pendingRestart_ = false;
        kind = Kind::wrap; // backward jump to a new motion, physics preserved
    }
    if (config_.hasMotion) {
        const double last = config_.lastFrame;
        double next = frame_ + seconds * kMotionFps;
        if (next > last) {
            if (config_.loop && last > 0.0) {
                next = std::fmod(next, last);
                ++loops_;
                kind = Kind::wrap;
            } else {
                next = last;
            }
        }
        frame_ = next;
    }
    return {kind, static_cast<float>(frame_), static_cast<float>(seconds), ticks};
}

} // namespace uka3d
