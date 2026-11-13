#include "app/SeatAnchor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace uka3d {
namespace {

using mmd::Float3;

std::optional<std::size_t> findBone(const mmd::PmxModel &model, std::string_view name) {
    if (name.empty())
        return std::nullopt;
    for (std::size_t i = 0; i < model.bones.size(); ++i)
        if (model.bones[i].name == name || model.bones[i].englishName == name)
            return i;
    return std::nullopt;
}

// Bone with the largest weight of a vertex (-1 if none).
std::int32_t dominantBone(const mmd::PmxVertex &vertex) {
    const std::size_t influences =
        vertex.weightType == mmd::PmxWeightType::bdef1
            ? 1
            : (vertex.weightType == mmd::PmxWeightType::bdef2 || vertex.weightType == mmd::PmxWeightType::sdef ? 2 : 4);
    std::int32_t best = -1;
    float bestWeight = 0.0F;
    for (std::size_t i = 0; i < influences; ++i) {
        if (vertex.bones[i] >= 0 && std::isfinite(vertex.weights[i]) && vertex.weights[i] > bestWeight) {
            best = vertex.bones[i];
            bestWeight = vertex.weights[i];
        }
    }
    return best;
}

struct ContactBone {
    std::size_t bone;
    // When set, only vertices on the hip side of this plane are used (upper thigh).
    std::optional<std::size_t> limitChild;
};

// Reduces a vertex cloud to its extreme points along evenly spread directions,
// which is enough to find the lowest point under any rotation.
void appendSupportPoints(const std::vector<Float3> &cloud, std::uint32_t bone, std::vector<Float3> &scratch,
                         std::vector<std::pair<std::uint32_t, Float3>> &out) {
    if (cloud.empty())
        return;
    constexpr int directions = 96;
    scratch.clear();
    const float golden = std::numbers::pi_v<float> * (3.0F - std::sqrt(5.0F));
    for (int i = 0; i < directions; ++i) {
        const float y = 1.0F - 2.0F * (static_cast<float>(i) + 0.5F) / directions;
        const float radius = std::sqrt(std::max(0.0F, 1.0F - y * y));
        const float angle = golden * static_cast<float>(i);
        const Float3 direction{std::cos(angle) * radius, y, std::sin(angle) * radius};
        float best = -std::numeric_limits<float>::infinity();
        const Float3 *extreme = nullptr;
        for (const auto &p : cloud) {
            const float d = p[0] * direction[0] + p[1] * direction[1] + p[2] * direction[2];
            if (d > best) {
                best = d;
                extreme = &p;
            }
        }
        if (extreme != nullptr &&
            std::ranges::none_of(scratch, [&](const Float3 &q) { return q == *extreme; }))
            scratch.push_back(*extreme);
    }
    for (const auto &p : scratch)
        out.emplace_back(bone, p);
}

} // namespace

const char *SeatAnchor::mode() const noexcept {
    if (!points_.empty())
        return "contact";
    return anchorBones_.empty() ? "none" : "bone";
}

void SeatAnchor::bind(const mmd::PmxModel &model, const SeatSettings &settings) {
    settings_ = settings;
    points_.clear();
    contactNames_.clear();
    anchorBones_.clear();
    anchorName_.clear();

    // ---- Horizontal anchor ---------------------------------------------------
    if (const auto bone = findBone(model, settings.anchorBone)) {
        anchorBones_ = {*bone};
        anchorName_ = model.bones[*bone].name;
    } else {
        const auto left = findBone(model, "左足");
        const auto right = findBone(model, "右足");
        if (left && right) {
            anchorBones_ = {*left, *right};
            anchorName_ = "左足+右足";
        } else {
            for (const char *name : {"下半身", "腰", "pelvis", "hips", "センター", "center"}) {
                if (const auto fallback = findBone(model, name)) {
                    anchorBones_ = {*fallback};
                    anchorName_ = model.bones[*fallback].name;
                    break;
                }
            }
        }
    }
    if (!settings.useContact)
        return;

    // ---- Contact bones ------------------------------------------------------------
    std::vector<ContactBone> contact;
    if (!settings.contactBones.empty()) {
        for (const auto &name : settings.contactBones)
            if (const auto bone = findBone(model, name))
                contact.push_back({*bone, std::nullopt});
    } else {
        for (const char *name : {"下半身", "腰", "pelvis", "hips"})
            if (const auto bone = findBone(model, name))
                contact.push_back({*bone, std::nullopt});
        // Thighs: semi-standard models weight the mesh to the D bones.
        const std::pair<const char *, const char *> thighs[] = {
            {"左足", "左ひざ"}, {"右足", "右ひざ"}, {"左足D", "左ひざD"}, {"右足D", "右ひざD"}};
        for (const auto &[thigh, knee] : thighs)
            if (const auto bone = findBone(model, thigh))
                contact.push_back({*bone, findBone(model, knee)});
    }
    if (contact.empty())
        return;

    std::vector<std::vector<Float3>> clouds(contact.size());
    for (const auto &vertex : model.vertices) {
        const auto bone = dominantBone(vertex);
        if (bone < 0)
            continue;
        for (std::size_t c = 0; c < contact.size(); ++c) {
            if (contact[c].bone != static_cast<std::size_t>(bone))
                continue;
            if (contact[c].limitChild) {
                // Keep the half of the thigh next to the hip: the knee end hangs
                // below the seat when the legs dangle.
                const auto &hip = model.bones[contact[c].bone].position;
                const auto &knee = model.bones[*contact[c].limitChild].position;
                const Float3 axis{knee[0] - hip[0], knee[1] - hip[1], knee[2] - hip[2]};
                const float lengthSq = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
                const float along = (vertex.position[0] - hip[0]) * axis[0] + (vertex.position[1] - hip[1]) * axis[1] +
                                    (vertex.position[2] - hip[2]) * axis[2];
                if (lengthSq > 1e-8F && along > 0.5F * lengthSq)
                    continue;
            }
            clouds[c].push_back(vertex.position);
        }
    }
    std::vector<Float3> scratch;
    std::vector<std::pair<std::uint32_t, Float3>> points;
    for (std::size_t c = 0; c < contact.size(); ++c) {
        if (clouds[c].empty())
            continue;
        appendSupportPoints(clouds[c], static_cast<std::uint32_t>(contact[c].bone), scratch, points);
        contactNames_.push_back(model.bones[contact[c].bone].name);
    }
    points_.reserve(points.size());
    for (const auto &[bone, position] : points)
        points_.push_back({bone, position});
}

mmd::Float3 SeatAnchor::evaluate(const mmd::PmxModel &model, std::span<const BoneTransform> bones) const {
    Float3 anchor{};
    if (!anchorBones_.empty()) {
        for (const auto bone : anchorBones_) {
            const auto p = jointPosition(model, bones, bone);
            for (std::size_t axis = 0; axis < 3; ++axis)
                anchor[axis] += p[axis] / static_cast<float>(anchorBones_.size());
        }
    }
    if (!points_.empty()) {
        float lowest = std::numeric_limits<float>::infinity();
        for (const auto &point : points_) {
            const float y = point.bone < bones.size() ? transformPoint(bones[point.bone], point.bind)[1] : point.bind[1];
            lowest = std::min(lowest, y);
        }
        if (std::isfinite(lowest))
            anchor[1] = lowest;
    }
    // Moving the reference point down/left inside the body lifts/shifts the character.
    anchor[0] -= settings_.offsetX;
    anchor[1] -= settings_.offsetY;
    return anchor;
}

} // namespace uka3d
