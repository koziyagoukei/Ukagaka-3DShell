#include "motion/MotionGenerator.h"

#include "mmd/Skinning.h"

#include <mmd/animation.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

namespace uka3d::motion {
namespace {

using mmd::Float3;
using mmd::Float4;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kArmDown = 72.0F; // resting upper arm, degrees below the horizontal

constexpr float rad(float degrees) {
    return degrees * kPi / 180.0F;
}
constexpr float deg(float radians) {
    return radians * 180.0F / kPi;
}
float smooth(float t) {
    t = std::clamp(t, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

// q = qy * qx * qz (MMD's YXZ order). In MMD's left-handed space with the
// character facing -Z: +X swings a hanging limb forward (and tips the spine
// back), -Z lowers the left (+X) arm, +Y turns the left shoulder forward.
Float4 quatEuler(float xDeg, float yDeg, float zDeg) {
    const float cx = std::cos(rad(xDeg) * 0.5F), sx = std::sin(rad(xDeg) * 0.5F);
    const float cy = std::cos(rad(yDeg) * 0.5F), sy = std::sin(rad(yDeg) * 0.5F);
    const float cz = std::cos(rad(zDeg) * 0.5F), sz = std::sin(rad(zDeg) * 0.5F);
    return {cy * sx * cz + sy * cx * sz, sy * cx * cz - cy * sx * sz, cy * cx * sz - sy * sx * cz,
            cy * cx * cz + sy * sx * sz};
}

std::optional<std::size_t> findBone(const mmd::PmxModel &model, std::initializer_list<const char *> names) {
    for (const char *name : names)
        for (std::size_t i = 0; i < model.bones.size(); ++i)
            if (model.bones[i].name == name)
                return i;
    return std::nullopt;
}

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

// Keys are written every frame or two, so plain linear interpolation is enough.
class Builder {
  public:
    explicit Builder(std::uint32_t lastFrame) {
        motion_.modelName = "uka3d generated";
        motion_.lastFrame = lastFrame;
    }
    void key(const std::string &bone, std::uint32_t frame, const Float4 &rotation, const Float3 &translation = {}) {
        if (bone.empty())
            return;
        mmd::VmdBoneKey k;
        k.name = bone;
        k.frame = frame;
        k.rotation = rotation;
        k.translation = translation;
        for (std::size_t axis = 0; axis < 4; ++axis) {
            k.interpolation[axis + 0] = 20;
            k.interpolation[axis + 4] = 20;
            k.interpolation[axis + 8] = 107;
            k.interpolation[axis + 12] = 107;
        }
        motion_.bones.push_back(std::move(k));
    }
    void legIk(bool enabled) {
        mmd::VmdIkKey key{};
        key.visible = true;
        for (const char *name : {"左足ＩＫ", "右足ＩＫ", "左足IK", "右足IK", "左つま先ＩＫ", "右つま先ＩＫ", "左つま先IK",
                                 "右つま先IK"})
            key.states.push_back({name, enabled});
        motion_.ik.push_back(std::move(key));
    }
    mmd::VmdMotion take() {
        return std::move(motion_);
    }

  private:
    mmd::VmdMotion motion_;
};

struct LegPose {
    float thigh{}, knee{}, ankle{}; // local X rotations, degrees
};

// Rotations for given world directions: thigh and shin as degrees from straight
// down towards the front, foot pitch with the toes up positive.
LegPose legFromAngles(const Leg &leg, float thighDeg, float shinDeg, float footDeg) {
    const float thigh = rad(thighDeg) - leg.thighAngle;
    const float shin = rad(shinDeg) - leg.shinAngle;
    return {deg(thigh), deg(shin - thigh), deg(rad(footDeg) - shin)};
}

// Two-bone solve in the sagittal plane, knee to the front.
LegPose solveLeg(const Leg &leg, float hipUp, float ankleForward, float ankleUp, float footDeg) {
    const float l1 = leg.thighLength, l2 = leg.shinLength;
    const float df = ankleForward - leg.hipForward;
    const float du = ankleUp - hipUp;
    const float distance = std::clamp(std::hypot(df, du), std::abs(l1 - l2) + 1e-3F, (l1 + l2) * 0.9995F);
    const float direction = std::atan2(df, -du);
    const float thigh =
        direction + std::acos(std::clamp((l1 * l1 + distance * distance - l2 * l2) / (2.0F * l1 * distance), -1.0F, 1.0F));
    const float shin =
        direction - std::acos(std::clamp((l2 * l2 + distance * distance - l1 * l1) / (2.0F * l2 * distance), -1.0F, 1.0F));
    return legFromAngles(leg, deg(thigh), deg(shin), footDeg);
}

void keyLeg(Builder &builder, const Leg &leg, std::uint32_t frame, const LegPose &pose) {
    builder.key(leg.thigh, frame, quatEuler(pose.thigh, 0.0F, 0.0F));
    builder.key(leg.knee, frame, quatEuler(pose.knee, 0.0F, 0.0F));
    builder.key(leg.ankle, frame, quatEuler(pose.ankle, 0.0F, 0.0F));
}

struct ArmPose {
    float swing{};     // degrees forward
    float down{kArmDown};
    float elbow{10.0F}; // flexion
};

void keyArms(Builder &builder, const Rig &rig, std::uint32_t frame, const ArmPose &left, const ArmPose &right) {
    if (!rig.hasArms)
        return;
    builder.key("左腕", frame, quatEuler(left.swing, 0.0F, -(left.down - rig.armDownLeft)));
    builder.key("右腕", frame, quatEuler(right.swing, 0.0F, right.down - rig.armDownRight));
    if (rig.hasElbows) {
        builder.key("左ひじ", frame, quatEuler(0.0F, left.elbow, 0.0F));
        builder.key("右ひじ", frame, quatEuler(0.0F, -right.elbow, 0.0F));
    }
}

struct FootState {
    float forward{}, up{}, pitch{}; // ankle joint; pitch in degrees, toes up positive
};

} // namespace

std::string Rig::describe() const {
    if (!valid)
        return "rig: unusable (" + problem + ")";
    return std::format(
        "rig: height {:.2f}, leg {:.2f} (thigh {:.2f} + shin {:.2f}), ankle height {:.2f}, sole {:.2f} front / {:.2f} back, "
        "ground {:.2f}\n     center='{}' legIK={} upper2={} elbows={} bind arms {:.0f} / {:.0f} deg below horizontal",
        height, legLength, left.thighLength, left.shinLength, left.ankleHeight, left.ball, left.heel, ground, center,
        hasLegIk, hasUpper2, hasElbows, armDownLeft, armDownRight);
}

Rig analyze(const mmd::PmxModel &model) {
    Rig rig;
    const auto has = [&](std::initializer_list<const char *> names) { return findBone(model, names).has_value(); };

    float lowest = std::numeric_limits<float>::infinity();
    float highest = -std::numeric_limits<float>::infinity();
    for (const auto &vertex : model.vertices) {
        lowest = std::min(lowest, vertex.position[1]);
        highest = std::max(highest, vertex.position[1]);
    }

    struct Extent {
        float low{std::numeric_limits<float>::infinity()};
        float front{-std::numeric_limits<float>::infinity()};
        float back{-std::numeric_limits<float>::infinity()};
        bool any{false};
    };
    const auto build = [&](const char *side, Leg &leg, Extent &extent) -> bool {
        const std::string s = side;
        const auto thigh = findBone(model, {(s + "足").c_str()});
        const auto knee = findBone(model, {(s + "ひざ").c_str()});
        const auto ankle = findBone(model, {(s + "足首").c_str()});
        if (!thigh || !knee || !ankle) {
            rig.problem = s + "足 / " + s + "ひざ / " + s + "足首 のいずれかがありません";
            return false;
        }
        leg.thigh = s + "足";
        leg.knee = s + "ひざ";
        leg.ankle = s + "足首";
        leg.ankleIndex = static_cast<std::int32_t>(*ankle);
        const auto &hip = model.bones[*thigh].position;
        const auto &kneePos = model.bones[*knee].position;
        const auto &anklePos = model.bones[*ankle].position;
        const float f1 = -(kneePos[2] - hip[2]), u1 = kneePos[1] - hip[1];
        const float f2 = -(anklePos[2] - kneePos[2]), u2 = anklePos[1] - kneePos[1];
        leg.thighLength = std::hypot(f1, u1);
        leg.shinLength = std::hypot(f2, u2);
        if (leg.thighLength < 1e-4F || leg.shinLength < 1e-4F) {
            rig.problem = s + "足の長さが 0 です";
            return false;
        }
        leg.thighAngle = std::atan2(f1, -u1);
        leg.shinAngle = std::atan2(f2, -u2);
        leg.hipForward = -hip[2];
        leg.hipUp = hip[1];
        leg.ankleForward = -anklePos[2];
        leg.ankleX = anklePos[0];

        // Sole shape from the vertices that follow the foot bones.
        // The ankle (and its semi-standard D copy) plus everything below it:
        // toe bones are named differently from model to model (足先EX, つま先EX, ...).
        std::vector<std::int32_t> footBones;
        for (const char *suffix : {"足首", "足首D"})
            if (const auto bone = findBone(model, {(s + suffix).c_str()}))
                footBones.push_back(static_cast<std::int32_t>(*bone));
        const std::size_t roots = footBones.size();
        for (std::size_t i = 0; i < model.bones.size(); ++i) {
            const auto self = static_cast<std::int32_t>(i);
            if (std::ranges::find(footBones, self) != footBones.end() || model.bones[i].ikLinks.size() > 0)
                continue;
            int guard = 0;
            for (auto parent = model.bones[i].parent; parent >= 0 && guard < 64; ++guard) {
                if (std::find(footBones.begin(), footBones.begin() + static_cast<std::ptrdiff_t>(roots), parent) !=
                    footBones.begin() + static_cast<std::ptrdiff_t>(roots)) {
                    footBones.push_back(self);
                    break;
                }
                parent = model.bones[static_cast<std::size_t>(parent)].parent;
            }
        }
        for (const auto &vertex : model.vertices) {
            const auto bone = dominantBone(vertex);
            if (std::ranges::find(footBones, bone) == footBones.end())
                continue;
            extent.any = true;
            extent.low = std::min(extent.low, vertex.position[1]);
            extent.front = std::max(extent.front, -vertex.position[2] - leg.ankleForward);
            extent.back = std::max(extent.back, leg.ankleForward + vertex.position[2]);
        }
        return true;
    };
    Extent leftExtent, rightExtent;
    if (!build("左", rig.left, leftExtent) || !build("右", rig.right, rightExtent))
        return rig;

    if (leftExtent.any && rightExtent.any)
        rig.ground = std::min(leftExtent.low, rightExtent.low);
    else
        rig.ground = std::isfinite(lowest) ? lowest : 0.0F;
    rig.height = std::isfinite(highest) ? highest - rig.ground : 20.0F;
    const auto finishFoot = [&](const char *side, Leg &leg, const Extent &extent) {
        const auto ankle = findBone(model, {(std::string(side) + "足首").c_str()});
        leg.ankleHeight = model.bones[*ankle].position[1] - rig.ground;
        const float legLength = leg.thighLength + leg.shinLength;
        if (leg.ankleHeight < legLength * 0.01F)
            leg.ankleHeight = legLength * 0.08F;
        const float front = extent.any ? extent.front : leg.ankleHeight * 1.5F;
        const float back = extent.any ? extent.back : leg.ankleHeight * 0.5F;
        leg.ball = std::clamp(front * 0.95F, leg.ankleHeight * 0.3F, legLength * 0.25F);
        leg.heel = std::clamp(back * 0.95F, leg.ankleHeight * 0.1F, legLength * 0.12F);
    };
    finishFoot("左", rig.left, leftExtent);
    finishFoot("右", rig.right, rightExtent);
    rig.legLength = 0.5F * (rig.left.thighLength + rig.left.shinLength + rig.right.thighLength + rig.right.shinLength);

    for (const char *name : {"センター", "center", "グルーブ", "全ての親"}) {
        if (has({name})) {
            rig.center = name;
            break;
        }
    }
    rig.hasGroove = has({"グルーブ"});
    rig.hasLower = has({"下半身"});
    rig.hasUpper = has({"上半身"});
    rig.hasUpper2 = has({"上半身2"});
    rig.hasNeck = has({"首"});
    rig.hasHead = has({"頭"});
    rig.hasLegIk = has({"左足ＩＫ", "左足IK"}) && has({"右足ＩＫ", "右足IK"});
    const auto armDown = [&](const char *arm, const char *elbow, const char *wrist, float &out) {
        const auto shoulder = findBone(model, {arm});
        const auto next = findBone(model, {elbow, wrist});
        if (!shoulder)
            return false;
        out = 0.0F;
        if (next) {
            const auto &a = model.bones[*shoulder].position;
            const auto &b = model.bones[*next].position;
            out = deg(std::atan2(-(b[1] - a[1]), std::abs(b[0] - a[0])));
        }
        return true;
    };
    const bool leftArm = armDown("左腕", "左ひじ", "左手首", rig.armDownLeft);
    const bool rightArm = armDown("右腕", "右ひじ", "右手首", rig.armDownRight);
    rig.hasArms = leftArm && rightArm;
    rig.hasElbows = has({"左ひじ"}) && has({"右ひじ"});
    rig.valid = true;
    return rig;
}

FootPoints footPoints(const Rig &rig, bool left) {
    const Leg &leg = left ? rig.left : rig.right;
    FootPoints points;
    points.bone = static_cast<std::size_t>(std::max(0, leg.ankleIndex));
    points.heel = {leg.ankleX, rig.ground, -(leg.ankleForward - leg.heel)};
    points.ball = {leg.ankleX, rig.ground, -(leg.ankleForward + leg.ball)};
    return points;
}

WalkParams fitWalk(const Rig &rig, float unitsPerSecond, float stepRatio) {
    WalkParams params;
    params.stepRatio = std::clamp(stepRatio, 0.15F, 0.75F);
    unitsPerSecond = std::max(unitsPerSecond, 1e-3F);
    // Natural step first, then the cycle length that matches the speed, then
    // the step that matches exactly with that whole number of frames.
    const float stride = 2.0F * params.stepRatio * rig.legLength;
    params.frames = static_cast<std::uint32_t>(std::clamp(std::lround(stride / unitsPerSecond * 30.0F), 20L, 60L));
    params.stepRatio = std::clamp(unitsPerSecond * static_cast<float>(params.frames) / 30.0F / (2.0F * rig.legLength),
                                  0.15F, 0.75F);
    return params;
}

ContactReport measureContact(const mmd::PmxModel &model, const Rig &rig, const mmd::VmdMotion &motion,
                             float walkSpeed) {
    mmd::MmdAnimator animator(model);
    animator.setMotion(&motion);
    Skinner skinner(model);
    std::vector<SkinnedVertex> vertices(model.vertices.size());
    mmd::AnimatedModelFrame frame;
    constexpr float step = 0.25F;
    // A foot counts as planted while its lowest sole point touches the floor.
    const float tolerance = 0.004F * rig.legLength;
    ContactReport report;
    double sum = 0.0;
    struct Previous {
        bool valid{false};
        bool ball{false};
        float position{};
    } previous[2];
    Float3 first[2][2]{};
    const float last = static_cast<float>(motion.lastFrame);
    for (float f = 0.0F; f <= last + 1e-3F; f += step) {
        animator.evaluateInto(frame, f, 0.0F, true, {}, mmd::VertexOutput::omitUnchangedGpu);
        skinner.skin(frame, vertices);
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const float depth = rig.ground - vertices[i].position[1];
            if (depth > report.penetration) {
                report.penetration = depth;
                report.deepestVertex = i;
                report.deepestFrame = f;
            }
        }
        for (int side = 0; side < 2; ++side) {
            const auto points = footPoints(rig, side == 0);
            if (points.bone >= frame.bones.size())
                continue;
            const auto heel = transformPoint(frame.bones[points.bone], points.heel);
            const auto ball = transformPoint(frame.bones[points.bone], points.ball);
            if (f == 0.0F) {
                first[side][0] = heel;
                first[side][1] = ball;
            }
            for (int p = 0; p < 2; ++p) {
                const auto &q = p == 0 ? heel : ball;
                report.drift = std::max(report.drift, std::hypot(q[0] - first[side][p][0], q[2] - first[side][p][2]));
            }
            const bool isBall = ball[1] < heel[1];
            const auto &contact = isBall ? ball : heel;
            if (contact[1] - rig.ground > tolerance) {
                previous[side].valid = false;
                continue;
            }
            // Position along the walking direction (-Z) as the body advances.
            const float position = -contact[2] + walkSpeed * f / 30.0F;
            if (previous[side].valid && previous[side].ball == isBall) {
                const float speed = std::abs(position - previous[side].position) / (step / 30.0F);
                if (speed > report.slideMax) {
                    report.slideMax = speed;
                    report.worstFrame = f;
                }
                sum += speed;
                ++report.samples;
            }
            previous[side] = {true, isBall, position};
        }
    }
    report.slideMean = report.samples ? static_cast<float>(sum / report.samples) : 0.0F;
    return report;
}

// ---------------------------------------------------------------------------
// Walk: heel strike -> flat -> heel rise -> swing. The contact point of a
// planted foot moves backwards at exactly the walking speed, so the foot does
// not slide when the character advances by `info.speed`.
// ---------------------------------------------------------------------------
mmd::VmdMotion makeWalk(const Rig &rig, const WalkParams &params, WalkInfo &info) {
    const std::uint32_t frames = std::max<std::uint32_t>(params.frames, 8);
    const float stance = std::clamp(params.stance, 0.5F, 0.75F);
    const float seconds = static_cast<float>(frames) / 30.0F;
    const float stride = 2.0F * params.stepRatio * rig.legLength;
    const float travel = stance * stride; // contact point travel during one stance
    info = {seconds, stride, stride / seconds};

    // Shorter steps roll the foot less (and bend the knee less).
    const float roll = std::clamp(params.stepRatio / 0.5F, 0.4F, 1.2F);
    const float strikeDeg = 16.0F * roll; // toes up at heel strike
    const float pushDeg = 26.0F * roll;   // heel up at toe off
    const float clearance = std::clamp(params.lift, 0.01F, 0.15F) * rig.legLength;

    const auto planted = [&](const Leg &leg, float s) {
        const float a = leg.ankleHeight, b = leg.ball, c = leg.heel;
        const float flat = leg.ankleForward + travel * (0.5F - s);
        FootState state{flat, rig.ground + a, 0.0F};
        if (s < 0.2F) {
            const float q = rad(strikeDeg) * (1.0F - smooth(s / 0.2F));
            state = {flat - c + c * std::cos(q) - a * std::sin(q), rig.ground + c * std::sin(q) + a * std::cos(q), deg(q)};
        } else if (s > 0.55F) {
            const float p = rad(pushDeg) * smooth((s - 0.55F) / 0.45F);
            state = {flat + b - b * std::cos(p) + a * std::sin(p), rig.ground + b * std::sin(p) + a * std::cos(p), -deg(p)};
        }
        return state;
    };
    const auto foot = [&](const Leg &leg, float phase) {
        phase -= std::floor(phase);
        if (phase < stance)
            return planted(leg, phase / stance);
        const float s = (phase - stance) / (1.0F - stance);
        const FootState from = planted(leg, 1.0F), to = planted(leg, 0.0F);
        // Hermite with the ground speed at both ends: the foot leaves and meets
        // the floor without a jump in velocity.
        const float slope = -travel * (1.0F - stance) / stance;
        const float s2 = s * s, s3 = s2 * s;
        FootState state;
        state.forward = (2 * s3 - 3 * s2 + 1) * from.forward + (s3 - 2 * s2 + s) * slope + (-2 * s3 + 3 * s2) * to.forward +
                        (s3 - s2) * slope;
        state.pitch = from.pitch + (to.pitch - from.pitch) * smooth(s * 1.25F);
        // Height from the lowest point of the sole, so turning the foot from
        // toe-off to heel strike never drags the toes or the heel on the floor.
        const float pitch = rad(state.pitch);
        const float a = leg.ankleHeight;
        const float ballUp = leg.ball * std::sin(pitch) - a * std::cos(pitch);
        const float heelUp = -leg.heel * std::sin(pitch) - a * std::cos(pitch);
        state.up = rig.ground + clearance * std::sin(kPi * s) - std::min(ballUp, heelUp);
        return state;
    };

    // Hip height: as high as both legs allow, dipping at double support.
    const float bob = 0.012F * rig.legLength;
    // Gait phase of a frame. With startCrossing the loop begins with the left
    // foot flat under the hip and the right foot passing it.
    const float phase0 = params.startCrossing ? 0.5F * stance : 0.0F;
    const auto phaseOf = [&](std::uint32_t frame) {
        const float t = static_cast<float>(frame % frames) / static_cast<float>(frames) + phase0;
        return t - std::floor(t);
    };
    const auto wave = [&](float t) { return std::cos(4.0F * kPi * t); };
    float base = -bob;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const float t = phaseOf(frame);
        for (const auto &[leg, offset] : {std::pair{&rig.left, 0.0F}, std::pair{&rig.right, 0.5F}}) {
            const FootState state = foot(*leg, t + offset);
            const float reach = (leg->thighLength + leg->shinLength) * 0.992F;
            const float df = state.forward - leg->hipForward;
            const float allowed = state.up + std::sqrt(std::max(0.0F, reach * reach - df * df)) - leg->hipUp;
            base = std::min(base, allowed + bob * wave(t));
        }
    }
    const bool canDrop = !rig.center.empty();

    Builder builder(frames);
    builder.legIk(false);
    for (std::uint32_t frame = 0; frame <= frames; ++frame) {
        const float t = phaseOf(frame);
        const float drop = canDrop ? base - bob * wave(t) : 0.0F;
        if (canDrop)
            builder.key(rig.center, frame, {0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, drop, 0.0F});
        for (const auto &[leg, offset] : {std::pair{&rig.left, 0.0F}, std::pair{&rig.right, 0.5F}}) {
            const FootState state = foot(*leg, t + offset);
            keyLeg(builder, *leg, frame, solveLeg(*leg, leg->hipUp + drop, state.forward, state.up, state.pitch));
        }
        const float swing = std::cos(2.0F * kPi * t); // +1: left foot in front
        if (rig.hasUpper)
            builder.key("上半身", frame, quatEuler(-3.0F, -4.0F * swing, 0.0F));
        if (rig.hasHead)
            builder.key("頭", frame, quatEuler(1.0F, 3.0F * swing, 0.0F));
        keyArms(builder, rig, frame, {-16.0F * swing, kArmDown, 12.0F - 6.0F * swing},
                {16.0F * swing, kArmDown, 12.0F + 6.0F * swing});
    }
    return builder.take();
}

// ---------------------------------------------------------------------------
// Idle: breathing, soft knees and a slow turn of the upper body. The legs are
// solved here instead of by the model's leg IK: with nearly straight legs the
// IK leaves the feet a little below the floor when the hips sink.
// ---------------------------------------------------------------------------
mmd::VmdMotion makeIdle(const Rig &rig) {
    constexpr std::uint32_t frames = 240; // 8 s
    Builder builder(frames);
    builder.legIk(false);
    for (std::uint32_t frame = 0; frame <= frames; frame += 3) {
        const float w = 2.0F * kPi * static_cast<float>(frame % frames) / static_cast<float>(frames);
        const float breath = std::sin(2.0F * w);
        if (!rig.center.empty()) {
            const float drop = -0.008F * rig.height - 0.002F * rig.height * (1.0F - std::cos(2.0F * w));
            builder.key(rig.center, frame, {0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, drop, 0.0F});
            for (const Leg *leg : {&rig.left, &rig.right})
                keyLeg(builder, *leg, frame,
                       solveLeg(*leg, leg->hipUp + drop, leg->ankleForward, rig.ground + leg->ankleHeight, 0.0F));
        }
        if (rig.hasUpper)
            builder.key("上半身", frame, quatEuler(-0.5F - 0.9F * breath, 1.5F * std::sin(w + 0.4F), -1.0F * std::sin(w)));
        if (rig.hasUpper2)
            builder.key("上半身2", frame, quatEuler(-0.7F * std::sin(2.0F * w - 0.6F), 0.0F, 0.0F));
        if (rig.hasNeck)
            builder.key("首", frame, quatEuler(0.5F * std::sin(2.0F * w - 0.9F), 0.0F, 0.0F));
        if (rig.hasHead)
            builder.key("頭", frame,
                        quatEuler(1.2F * std::sin(2.0F * w + 0.3F), 5.0F * std::sin(w + 0.5F), 2.0F * std::sin(w + 1.2F)));
        const float arm = 1.2F * std::sin(2.0F * w - 0.8F);
        keyArms(builder, rig, frame, {2.0F * std::sin(w), kArmDown + arm, 10.0F}, {-2.0F * std::sin(w), kArmDown + arm, 10.0F});
    }
    return builder.take();
}

// ---------------------------------------------------------------------------
// Sit: thighs level, shins hanging and swinging a little, hands by the hips.
// ---------------------------------------------------------------------------
mmd::VmdMotion makeSit(const Rig &rig) {
    constexpr std::uint32_t frames = 180; // 6 s
    Builder builder(frames);
    builder.legIk(false);
    for (std::uint32_t frame = 0; frame <= frames; frame += 3) {
        const float w = 2.0F * kPi * static_cast<float>(frame % frames) / static_cast<float>(frames);
        const float breath = std::sin(2.0F * w);
        const float swingLeft = std::sin(2.0F * w), swingRight = std::sin(2.0F * w + 2.6F);
        keyLeg(builder, rig.left, frame, legFromAngles(rig.left, 88.0F, 4.0F + 7.0F * swingLeft, -14.0F + 4.0F * swingLeft));
        keyLeg(builder, rig.right, frame,
               legFromAngles(rig.right, 88.0F, 4.0F + 7.0F * swingRight, -14.0F + 4.0F * swingRight));
        if (rig.hasUpper)
            builder.key("上半身", frame, quatEuler(-5.0F - 0.9F * breath, 0.0F, 0.8F * std::sin(w)));
        if (rig.hasUpper2)
            builder.key("上半身2", frame, quatEuler(2.0F - 0.6F * std::sin(2.0F * w - 0.6F), 0.0F, 0.0F));
        if (rig.hasHead)
            builder.key("頭", frame, quatEuler(2.0F, 4.0F * std::sin(w), 1.5F * std::sin(w + 1.0F)));
        const float arm = 1.0F * std::sin(2.0F * w - 0.8F);
        keyArms(builder, rig, frame, {-6.0F, 80.0F + arm, 8.0F}, {-6.0F, 80.0F + arm, 8.0F});
    }
    return builder.take();
}

// ---------------------------------------------------------------------------
// Grab (held up while dragged): the body hangs and swings like a pendulum,
// the limbs follow a little later.
// ---------------------------------------------------------------------------
mmd::VmdMotion makeGrab(const Rig &rig) {
    constexpr std::uint32_t frames = 72; // 2.4 s
    Builder builder(frames);
    builder.legIk(false);
    for (std::uint32_t frame = 0; frame <= frames; frame += 2) {
        const float w = 2.0F * kPi * static_cast<float>(frame % frames) / static_cast<float>(frames);
        const float body = std::sin(w), late = std::sin(w - 0.7F), later = std::sin(w - 1.2F);
        if (!rig.center.empty())
            builder.key(rig.center, frame, quatEuler(0.0F, 0.0F, 5.0F * body));
        if (rig.hasUpper)
            builder.key("上半身", frame, quatEuler(-7.0F, 0.0F, -2.5F * late));
        if (rig.hasHead)
            builder.key("頭", frame, quatEuler(-9.0F, 0.0F, -4.0F * later));
        keyLeg(builder, rig.left, frame, legFromAngles(rig.left, 14.0F + 4.0F * late, -14.0F + 6.0F * later, -26.0F));
        keyLeg(builder, rig.right, frame, legFromAngles(rig.right, 5.0F - 4.0F * late, -30.0F - 6.0F * later, -30.0F));
        keyArms(builder, rig, frame, {8.0F + 3.0F * late, 58.0F + 3.0F * late, 26.0F},
                {12.0F - 3.0F * late, 54.0F - 3.0F * late, 30.0F});
    }
    return builder.take();
}

} // namespace uka3d::motion
