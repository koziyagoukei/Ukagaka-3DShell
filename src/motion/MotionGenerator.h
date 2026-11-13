#pragma once

#include <mmd/pmx.hpp>
#include <mmd/vmd.hpp>

#include <cstdint>
#include <string>

// Procedural motions fitted to one model.
//
// VMD keys are plain rotations and translations, so a walk made for one
// character slides or sinks on another one with different leg lengths. The
// generator therefore measures the legs, feet and arms of the PMX and solves
// the poses for it: feet stay where they are planted, the hips never ask for
// more reach than the legs have, and the arms hang at the same angle whether
// the model was built in a T or an A pose. Everything it writes is original
// data (no third-party motion is involved).
namespace uka3d::motion {

struct Leg {
    std::string thigh, knee, ankle; // bone names used for the keys
    std::int32_t ankleIndex{-1};
    float thighLength{};   // in the sagittal (forward/up) plane
    float shinLength{};
    float thighAngle{};    // bind direction, radians from straight down towards the front
    float shinAngle{};
    float hipForward{};    // bind hip joint, forward (-Z) / up
    float hipUp{};
    float ankleForward{};  // bind ankle joint
    float ankleHeight{};   // ankle joint above the sole
    float ball{};          // sole contact under the toes, in front of the ankle
    float heel{};          // sole contact behind the ankle
    float ankleX{};        // lateral position (bind)
};

struct Rig {
    bool valid{false};
    std::string problem; // why the rig is unusable
    Leg left, right;
    float ground{};    // sole height (model space)
    float height{};    // top of the model above the ground
    float legLength{}; // mean thigh + shin
    std::string center;       // translated for the hip height ("センター")
    bool hasGroove{false}, hasLower{false}, hasUpper{false}, hasUpper2{false}, hasNeck{false}, hasHead{false};
    bool hasArms{false}, hasElbows{false}, hasLegIk{false};
    float armDownLeft{};  // degrees the bind upper arm points below the horizontal
    float armDownRight{};
    [[nodiscard]] std::string describe() const;
};

[[nodiscard]] Rig analyze(const mmd::PmxModel &model);

struct WalkParams {
    std::uint32_t frames{30}; // one cycle (two steps) at 30 fps
    float stepRatio{0.5F};    // step length / leg length
    float stance{0.6F};       // part of the cycle a foot is on the ground
    float lift{0.055F};       // swing foot clearance / leg length
    // Where the loop starts in the gait cycle. false: left heel strike (feet
    // furthest apart). true: the legs passing each other, so the cross-fade
    // from standing does not spread the legs suddenly (calmer skirts).
    bool startCrossing{false};
};

struct WalkInfo {
    float seconds{}; // cycle duration
    float stride{};  // distance per cycle (model units)
    float speed{};   // model units per second at which the feet do not slide
};

// Cycle length and step for a walk whose planted foot moves at `unitsPerSecond`.
// `stepRatio` is the preferred step (step length / leg length): smaller steps
// keep long skirts and robes down; the cycle gets longer instead.
[[nodiscard]] WalkParams fitWalk(const Rig &rig, float unitsPerSecond, float stepRatio = 0.5F);

[[nodiscard]] mmd::VmdMotion makeIdle(const Rig &rig);
[[nodiscard]] mmd::VmdMotion makeWalk(const Rig &rig, const WalkParams &params, WalkInfo &info);
[[nodiscard]] mmd::VmdMotion makeSit(const Rig &rig);
[[nodiscard]] mmd::VmdMotion makeGrab(const Rig &rig);

// Sole contact points in bind space (attached to the ankle bone), for checks.
struct FootPoints {
    std::size_t bone{};
    mmd::Float3 heel{}, ball{};
};
[[nodiscard]] FootPoints footPoints(const Rig &rig, bool left);

// Plays `motion` on the model (no physics) and measures how well the feet
// stay on the floor. `walkSpeed` is the speed the character advances at
// (model units per second, 0 for motions in place).
struct ContactReport {
    float slideMax{};    // fastest movement of a planted foot, units/s
    float slideMean{};
    int samples{};       // planted samples
    float penetration{}; // deepest vertex below the floor
    float drift{};       // largest horizontal movement of the sole points
    float worstFrame{};  // where slideMax happened
    std::size_t deepestVertex{}; // vertex that went furthest below the floor
    float deepestFrame{};
};
[[nodiscard]] ContactReport measureContact(const mmd::PmxModel &model, const Rig &rig, const mmd::VmdMotion &motion,
                                           float walkSpeed);

} // namespace uka3d::motion
