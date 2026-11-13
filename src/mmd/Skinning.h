#pragma once

#include <mmd/animation.hpp>
#include <mmd/pmx.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace uka3d {

using BoneTransform = mmd::AnimatedModelFrame::BoneTransform;

// Layout matches the renderer's dynamic vertex stream (position, normal).
struct SkinnedVertex {
    float position[3];
    float normal[3];
};

// CPU skinning from libmmd's bone transforms.
//
// libmmd's own CPU path copies every PmxVertex (172 bytes each) on every tick
// and transforms each influence with quaternion arithmetic. This class asks
// libmmd only for the bone transforms and effective vertex-morph weights
// (`gpuSkinning = true`) and deforms compact arrays with per-bone matrices.
// The result is the same deformation: BDEF1/2/4 linear blending, SDEF and
// QDEF use libmmd's formulas (see tests: every vertex is compared).
class Skinner {
  public:
    explicit Skinner(const mmd::PmxModel &model);

    // `bones` are skinning transforms (p' = rotate(q, p) + t), one per model bone.
    // `morphWeights` are effective vertex-morph weights, one per model morph (may be empty).
    void skin(std::span<const BoneTransform> bones, std::span<const float> morphWeights,
              std::span<SkinnedVertex> out);
    void skin(const mmd::AnimatedModelFrame &frame, std::span<SkinnedVertex> out) {
        skin(frame.bones, frame.morphWeights, out);
    }

    [[nodiscard]] std::size_t vertexCount() const noexcept {
        return influenceCount_.size();
    }

  private:
    struct Matrix34 {
        float m[12]; // row-major 3x4: [r00 r01 r02 tx | r10 .. ty | r20 .. tz]
    };
    struct Sdef {
        std::uint32_t vertex;
        std::uint32_t bone0, bone1;
        float weight;
        mmd::Float3 c, cr0, cr1;
    };
    struct Qdef {
        std::uint32_t vertex;
    };

    void applyMorphs(std::span<const float> morphWeights);
    void skinLinear(std::size_t vertex, SkinnedVertex &out) const;

    const mmd::PmxModel &model_;
    // Bind pose with vertex morphs applied.
    std::vector<mmd::Float3> position_;
    std::vector<mmd::Float3> normal_;
    // Linear-blend influences, normalised at construction (count 0 = not skinned).
    std::vector<std::uint8_t> influenceCount_;
    std::vector<std::uint32_t> influenceBone_; // 4 per vertex
    std::vector<float> influenceWeight_;       // 4 per vertex
    std::vector<Sdef> sdef_;
    std::vector<Qdef> qdef_;
    // Vertex morphs.
    std::vector<std::uint32_t> vertexMorphs_; // indices of usable type-1 morphs
    std::vector<float> appliedWeights_;       // per model morph
    std::vector<std::uint32_t> dirtyVertices_;
    std::vector<std::uint8_t> dirtyFlag_;
    bool morphed_{false};
    // Scratch.
    std::vector<Matrix34> matrices_;
};

// ---------------------------------------------------------------------------
// Pose blending
// ---------------------------------------------------------------------------

// Blends two sets of skinning transforms in parent-local space, so limbs keep
// their length while rotating (a plain per-vertex or world-space blend would
// shorten them). weight 0 = `from`, 1 = `to`.
class PoseBlender {
  public:
    explicit PoseBlender(const mmd::PmxModel &model);
    void blend(std::span<const BoneTransform> from, std::span<const BoneTransform> to, float weight,
               std::vector<BoneTransform> &out) const;

  private:
    const mmd::PmxModel &model_;
    std::vector<std::uint32_t> order_; // parents before children
};

// Linear interpolation between two consecutive simulation ticks (small differences).
void lerpBones(std::span<const BoneTransform> from, std::span<const BoneTransform> to, float weight,
               std::vector<BoneTransform> &out);

// Animated joint position (model space) of bone `index`.
mmd::Float3 jointPosition(const mmd::PmxModel &model, std::span<const BoneTransform> bones, std::size_t index);
// Transforms a bind-space point attached to bone `index`.
mmd::Float3 transformPoint(const BoneTransform &bone, const mmd::Float3 &bindPoint);

} // namespace uka3d
