#pragma once

#include "mmd/Skinning.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uka3d {

// Per-model seating adjustments (configure_seat / "seat" in ukagaka3d.json).
struct SeatSettings {
    // Horizontal reference. Empty: midpoint of the thigh joints, then the hip bones.
    std::string anchorBone;
    // Bones whose surface may touch the seat. Empty: pelvis and the upper half of both thighs.
    std::vector<std::string> contactBones;
    // false: use the anchor bone's joint height instead of the body surface (Phase 2 behaviour).
    bool useContact{true};
    float offsetX{0.0F};     // model units, positive moves the character towards its left (+X, screen right)
    float offsetY{0.0F};     // model units, positive lifts the character above the edge
    float yawDegrees{20.0F}; // camera yaw while seated
    int insetPixels{40};     // distance from the window corner at 96 dpi (top-left / top-right)
    int transitionMs{350};   // pose cross-fade and hop duration; 0 = instant
};

// Finds the point of the character that rests on the seat.
//
// The hip joint sits in the middle of the pelvis, so aligning it with a window
// edge sinks the character by the thickness of the buttocks. Instead, a small
// set of surface points of the pelvis and upper thighs is picked when the
// model is bound; at run time only those points are transformed with the
// current pose and the lowest one defines the seat height. Works for models
// without a "腰" bone and needs no per-frame vertex access.
class SeatAnchor {
  public:
    void bind(const mmd::PmxModel &model, const SeatSettings &settings);

    // Seat reference in model space: x/z from the anchor, y from the lowest contact point.
    [[nodiscard]] mmd::Float3 evaluate(const mmd::PmxModel &model, std::span<const BoneTransform> bones) const;
    // Calls `visit` with every posed contact point (offsets applied). With a
    // perspective camera the point that looks lowest on screen is not always
    // the lowest one in model space, so callers project them individually.
    template <typename Visit>
    void forEachContact(std::span<const BoneTransform> bones, Visit &&visit) const {
        for (const auto &point : points_) {
            auto p = point.bone < bones.size() ? transformPoint(bones[point.bone], point.bind) : point.bind;
            p[0] -= settings_.offsetX;
            p[1] -= settings_.offsetY;
            visit(p);
        }
    }

    [[nodiscard]] const SeatSettings &settings() const noexcept {
        return settings_;
    }
    [[nodiscard]] std::size_t contactPointCount() const noexcept {
        return points_.size();
    }
    [[nodiscard]] const std::vector<std::string> &contactBoneNames() const noexcept {
        return contactNames_;
    }
    // "contact" (surface points), "bone" (joint height only) or "none" (camera target fallback).
    [[nodiscard]] const char *mode() const noexcept;
    [[nodiscard]] const std::string &anchorName() const noexcept {
        return anchorName_;
    }

  private:
    struct Point {
        std::uint32_t bone;
        mmd::Float3 bind; // bind-space position of a surface vertex
    };
    SeatSettings settings_;
    std::vector<Point> points_;
    std::vector<std::string> contactNames_;
    std::vector<std::size_t> anchorBones_; // 1 bone, or the two thigh joints (midpoint)
    std::string anchorName_;
};

} // namespace uka3d
