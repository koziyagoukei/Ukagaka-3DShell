#include "app/Face.h"

#include <algorithm>
#include <cmath>

namespace uka3d {

namespace facemath {

Quat multiply(const Quat &a, const Quat &b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Quat conjugate(const Quat &q) {
    return {-q[0], -q[1], -q[2], q[3]};
}

mmd::Float3 rotate(const Quat &q, const mmd::Float3 &v) {
    const Quat p = multiply(multiply(q, Quat{v[0], v[1], v[2], 0.0F}), conjugate(q));
    return {p[0], p[1], p[2]};
}

Quat between(const mmd::Float3 &from, const mmd::Float3 &to) {
    const float dot = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
    if (dot < -0.9999F) // opposite: turn around the vertical axis
        return {0.0F, 1.0F, 0.0F, 0.0F};
    Quat q{from[1] * to[2] - from[2] * to[1], from[2] * to[0] - from[0] * to[2], from[0] * to[1] - from[1] * to[0],
           1.0F + dot};
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    return {q[0] / n, q[1] / n, q[2] / n, q[3] / n};
}

Quat slerp(const Quat &a, const Quat &bIn, float t) {
    Quat b = bIn;
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0F) {
        b = {-b[0], -b[1], -b[2], -b[3]};
        dot = -dot;
    }
    Quat r;
    if (dot > 0.9995F) {
        for (int i = 0; i < 4; ++i)
            r[i] = a[i] + (b[i] - a[i]) * t;
    } else {
        const float theta = std::acos(std::clamp(dot, -1.0F, 1.0F));
        const float s = std::sin(theta);
        const float wa = std::sin((1.0F - t) * theta) / s;
        const float wb = std::sin(t * theta) / s;
        for (int i = 0; i < 4; ++i)
            r[i] = wa * a[i] + wb * b[i];
    }
    const float n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    return {r[0] / n, r[1] / n, r[2] / n, r[3] / n};
}

float angleDegrees(const Quat &q) {
    return 2.0F * std::acos(std::clamp(std::abs(q[3]), 0.0F, 1.0F)) * 57.29578F;
}

} // namespace facemath

using namespace facemath;

// ---------------------------------------------------------------------------
// Blink
// ---------------------------------------------------------------------------

BlinkScheduler::BlinkScheduler(const BlinkSettings &settings, std::uint32_t seed)
    : settings_(settings), random_(seed) {
    scheduleNext(false);
}

void BlinkScheduler::setSettings(const BlinkSettings &settings) {
    settings_ = settings;
    if (phase_ == Phase::open)
        scheduleNext(false);
}

void BlinkScheduler::scheduleNext(bool allowDouble) {
    phase_ = Phase::open;
    t_ = 0.0;
    if (allowDouble && doublePending_) {
        doublePending_ = false;
        wait_ = 0.12;
        return;
    }
    const float lo = std::max(0.3F, std::min(settings_.minInterval, settings_.maxInterval));
    const float hi = std::max(lo, settings_.maxInterval);
    wait_ = std::uniform_real_distribution<double>(lo, hi)(random_);
    doublePending_ = std::uniform_real_distribution<double>(0.0, 1.0)(random_) < settings_.doubleChance;
}

void BlinkScheduler::hold() {
    if (phase_ == Phase::open)
        wait_ = std::max(wait_, t_ + 1.0); // at least a second from now
}

float BlinkScheduler::update(double dt) {
    if (!settings_.enabled) {
        phase_ = Phase::open;
        return 0.0F;
    }
    t_ += dt;
    switch (phase_) {
    case Phase::open:
        if (t_ < wait_)
            return 0.0F;
        phase_ = Phase::closing;
        t_ -= wait_;
        [[fallthrough]];
    case Phase::closing:
        if (t_ < settings_.closeSeconds)
            return static_cast<float>(t_ / settings_.closeSeconds);
        phase_ = Phase::closed;
        t_ -= settings_.closeSeconds;
        [[fallthrough]];
    case Phase::closed:
        if (t_ < settings_.holdSeconds)
            return 1.0F;
        phase_ = Phase::opening;
        t_ -= settings_.holdSeconds;
        [[fallthrough]];
    case Phase::opening:
        if (t_ < settings_.openSeconds) {
            const float u = static_cast<float>(t_ / settings_.openSeconds);
            return 1.0F - u * u * (3.0F - 2.0F * u);
        }
        scheduleNext(true);
        return 0.0F;
    }
    return 0.0F;
}

// ---------------------------------------------------------------------------
// Gaze
// ---------------------------------------------------------------------------

Quat gazeRotation(const Quat &headRotation, const mmd::Float3 &toward, float maxYawDegrees, float maxPitchDegrees) {
    const float len = std::sqrt(toward[0] * toward[0] + toward[1] * toward[1] + toward[2] * toward[2]);
    if (!(len > 1e-6F))
        return kIdentity;
    const mmd::Float3 world{toward[0] / len, toward[1] / len, toward[2] / len};
    // Into the head's frame, where it faces -Z.
    const mmd::Float3 d = rotate(conjugate(headRotation), world);
    constexpr float kDeg = 0.01745329F;
    float yaw = std::atan2(d[0], -d[2]);
    float pitch = std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2]));
    yaw = std::clamp(yaw, -maxYawDegrees * kDeg, maxYawDegrees * kDeg);
    pitch = std::clamp(pitch, -maxPitchDegrees * kDeg, maxPitchDegrees * kDeg);
    const mmd::Float3 clamped{std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    return between({0.0F, 0.0F, -1.0F}, clamped);
}

mmd::Float3 gazeTarget(const mmd::Float3 &cameraEye, const mmd::Float3 &rayDirection, const mmd::Float3 &eyes) {
    const float dx = eyes[0] - cameraEye[0], dy = eyes[1] - cameraEye[1], dz = eyes[2] - cameraEye[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5F;
    return {cameraEye[0] + rayDirection[0] * distance, cameraEye[1] + rayDirection[1] * distance,
            cameraEye[2] + rayDirection[2] * distance};
}

bool GazeFollower::update(double dt, bool active, const Quat &wanted) {
    const Quat before = current_;
    const float weightBefore = weight_;
    if (!settings_.enabled)
        active = false;
    if (active) {
        const float k = 1.0F - static_cast<float>(std::exp(-dt / std::max(0.01F, settings_.followSeconds)));
        current_ = slerp(current_, wanted, k);
        weight_ = std::min(1.0F, weight_ + static_cast<float>(dt / std::max(0.01F, settings_.followSeconds * 2.0F)));
    } else {
        weight_ = std::max(0.0F, weight_ - static_cast<float>(dt / std::max(0.01F, settings_.releaseSeconds)));
        if (weight_ == 0.0F)
            current_ = kIdentity;
    }
    float change = std::abs(weight_ - weightBefore);
    for (int i = 0; i < 4; ++i)
        change = std::max(change, std::abs(current_[i] - before[i]));
    return change > 1e-5F;
}

void GazeFollower::reset() {
    current_ = kIdentity;
    weight_ = 0.0F;
}

Quat GazeFollower::part(float share) const {
    // Smooth fade: the weight goes through smoothstep so the start and end are soft.
    const float w = weight_ * weight_ * (3.0F - 2.0F * weight_);
    return slerp(kIdentity, current_, std::clamp(share * w, 0.0F, 1.0F));
}

} // namespace uka3d
