// Tests for the seating/performance work: skinning equivalence with libmmd,
// pose blending, seat anchor and the camera's screen offset.
//   uka3d_quality_tests <test-assets-dir> [<extra model.pmx> <extra motion.vmd>]
// The optional extra model is checked for skinning equivalence too (used
// locally with real models; nothing from it is written anywhere).
#include "app/SeatAnchor.h"
#include "mmd/MmdModel.h"
#include "mmd/Skinning.h"
#include "motion/MotionGenerator.h"
#include "renderer/Camera.h"

#include <algorithm>

#include <mmd/animation.hpp>
#include <mmd/physics.hpp>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace uka3d;

namespace {

int g_failures = 0;
int g_checks = 0;

void report(bool ok, const char *expr, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAILED: %s (line %d)\n", expr, line);
    }
}
#define CHECK(expr) report(static_cast<bool>(expr), #expr, __LINE__)

float distance(const mmd::Float3 &a, const mmd::Float3 &b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

struct Difference {
    float position{};
    float normal{};
};

// Largest difference between libmmd's CPU-skinned vertices and the Skinner.
Difference compare(const mmd::AnimatedModelFrame &reference, const std::vector<SkinnedVertex> &skinned) {
    Difference d;
    for (std::size_t i = 0; i < reference.vertices.size() && i < skinned.size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            d.position = std::max(d.position, std::abs(reference.vertices[i].position[c] - skinned[i].position[c]));
            d.normal = std::max(d.normal, std::abs(reference.vertices[i].normal[c] - skinned[i].normal[c]));
        }
    }
    return d;
}

// Evaluates the same frames through both paths (physics off, so the bones are identical).
Difference equivalence(const mmd::PmxModel &model, const mmd::VmdMotion *motion, float lastFrame,
                       const std::vector<mmd::MorphOverride> &overrides) {
    mmd::MmdAnimator reference(model);
    mmd::MmdAnimator pose(model);
    if (motion != nullptr) {
        reference.setMotion(motion);
        pose.setMotion(motion);
    }
    Skinner skinner(model);
    std::vector<SkinnedVertex> skinned(model.vertices.size());
    mmd::AnimatedModelFrame full, bones;
    Difference worst;
    for (float frame = 0.0F; frame <= lastFrame; frame += std::max(1.0F, lastFrame / 12.0F)) {
        reference.evaluateInto(full, frame, 0.0F, false, overrides);
        pose.evaluateInto(bones, frame, 0.0F, true, overrides, mmd::VertexOutput::omitUnchangedGpu);
        skinner.skin(bones, skinned);
        const auto d = compare(full, skinned);
        worst.position = std::max(worst.position, d.position);
        worst.normal = std::max(worst.normal, d.normal);
        if (lastFrame <= 0.0F)
            break;
    }
    return worst;
}

void testSkinningEquivalence(const fs::path &assets) {
    const auto model = mmd::pmx::load(assets / L"テスト用モデル" / L"物理テストキャラ.pmx");
    std::vector<mmd::MorphOverride> smile;
    for (std::size_t i = 0; i < model.morphs.size(); ++i)
        if (model.morphs[i].name == "笑い")
            smile.push_back({i, 0.7F, model.morphs[i].type, {}});
    CHECK(!smile.empty());

    for (const wchar_t *name : {L"待機テスト.vmd", L"挨拶.vmd", L"着席.vmd", L"歩行.vmd", L"つまみ.vmd"}) {
        const auto motion = mmd::loadVmd(assets / L"モーション" / name);
        const auto d = equivalence(model, &motion, static_cast<float>(motion.lastFrame), smile);
        std::printf("    %ls: max position diff %.2e, normal diff %.2e\n", name, d.position, d.normal);
        CHECK(d.position < 1e-4F && d.normal < 1e-4F);
    }

    // The synthetic model only has BDEF1 vertices. Rewrite the weights so that
    // BDEF2, BDEF4, SDEF and QDEF (and their fallbacks) are exercised as well.
    auto mixed = model;
    std::mt19937 random(1234);
    std::uniform_int_distribution<int> bone(0, static_cast<int>(mixed.bones.size()) - 1);
    std::uniform_real_distribution<float> unit(0.05F, 0.95F);
    std::uniform_real_distribution<float> spread(-0.6F, 0.6F);
    std::size_t counts[5]{};
    for (std::size_t i = 0; i < mixed.vertices.size(); ++i) {
        auto &v = mixed.vertices[i];
        const int first = v.bones[0];
        switch (i % 6) {
        case 0:
            break; // BDEF1
        case 1: {
            v.weightType = mmd::PmxWeightType::bdef2;
            const float w = unit(random);
            v.bones = {first, bone(random), -1, -1};
            v.weights = {w, 1.0F - w, 0.0F, 0.0F};
            break;
        }
        case 2: {
            v.weightType = mmd::PmxWeightType::bdef4;
            v.bones = {first, bone(random), bone(random), -1}; // one unused slot
            v.weights = {0.5F, 0.3F, 0.4F, 0.0F};               // not normalised on purpose
            break;
        }
        case 3: {
            v.weightType = mmd::PmxWeightType::sdef;
            const float w = unit(random);
            v.bones = {first, bone(random), -1, -1};
            v.weights = {w, 1.0F - w, 0.0F, 0.0F};
            v.sdefC = {v.position[0] + spread(random), v.position[1] + spread(random), v.position[2]};
            v.sdefR0 = {v.sdefC[0], v.sdefC[1] + 0.5F, v.sdefC[2]};
            v.sdefR1 = {v.sdefC[0], v.sdefC[1] - 0.5F, v.sdefC[2]};
            break;
        }
        case 4: {
            v.weightType = mmd::PmxWeightType::qdef;
            v.bones = {first, bone(random), bone(random), bone(random)};
            v.weights = {0.4F, 0.3F, 0.2F, 0.1F};
            break;
        }
        default: {
            v.weightType = mmd::PmxWeightType::sdef; // invalid second bone: falls back to linear
            v.bones = {first, -1, -1, -1};
            v.weights = {1.0F, 0.0F, 0.0F, 0.0F};
            break;
        }
        }
        ++counts[static_cast<std::size_t>(v.weightType)];
    }
    CHECK(counts[1] > 0 && counts[2] > 0 && counts[3] > 0 && counts[4] > 0);
    for (const wchar_t *name : {L"挨拶.vmd", L"つまみ.vmd"}) {
        const auto motion = mmd::loadVmd(assets / L"モーション" / name);
        const auto d = equivalence(mixed, &motion, static_cast<float>(motion.lastFrame), smile);
        std::printf("    mixed weights, %ls: max position diff %.2e, normal diff %.2e\n", name, d.position, d.normal);
        CHECK(d.position < 2e-4F && d.normal < 2e-4F);
    }

    // Morph weights: applying, changing and removing must always match a fresh evaluation.
    {
        mmd::MmdAnimator reference(model);
        mmd::MmdAnimator pose(model);
        Skinner skinner(model);
        std::vector<SkinnedVertex> skinned(model.vertices.size());
        mmd::AnimatedModelFrame full, bones;
        float worst = 0.0F;
        for (float weight : {1.0F, 0.25F, 0.0F, 0.6F, 0.0F}) {
            std::vector<mmd::MorphOverride> overrides;
            if (weight > 0.0F)
                overrides.push_back({smile[0].index, weight, smile[0].type, {}});
            reference.evaluateInto(full, 0.0F, 0.0F, false, overrides);
            pose.evaluateInto(bones, 0.0F, 0.0F, true, overrides, mmd::VertexOutput::omitUnchangedGpu);
            skinner.skin(bones, skinned);
            worst = std::max(worst, compare(full, skinned).position);
        }
        CHECK(worst < 1e-5F);
    }
}

// MmdModel's pose-only path with physics: same simulation, same vertices.
void testPoseOnlyWithPhysics(const fs::path &assets) {
    const fs::path modelPath = assets / L"テスト用モデル" / L"物理テストキャラ.pmx";
    const fs::path motionPath = assets / L"モーション" / L"待機テスト.vmd";
    MmdModel full, fast;
    for (MmdModel *m : {&full, &fast}) {
        m->loadModel(modelPath);
        m->loadMotion(motionPath);
        CHECK(m->setupPhysics({}) == PhysicsStatus::active);
    }
    CHECK(fast.poseOnlySupported());
    std::vector<SkinnedVertex> skinned(fast.model().vertices.size());
    float worst = 0.0F;
    float frame = 0.0F;
    for (int tick = 0; tick < 150; ++tick) {
        const float seconds = tick == 0 ? 0.0F : 1.0F / 60.0F;
        const auto &reference = full.evaluate(frame, seconds);
        const auto &pose = fast.evaluatePose(frame, seconds);
        CHECK(pose.vertices.empty()); // libmmd did not copy the vertex array
        fast.skinner().skin(pose, skinned);
        worst = std::max(worst, compare(reference, skinned).position);
        frame += 0.5F;
    }
    // Loop wrap keeps the physics state in both paths.
    const auto &reference = full.evaluateAcrossLoop(0.0F, 1.0F / 60.0F);
    const auto &pose = fast.evaluatePoseAcrossLoop(0.0F, 1.0F / 60.0F);
    fast.skinner().skin(pose, skinned);
    worst = std::max(worst, compare(reference, skinned).position);
    std::printf("    physics, 150 ticks + loop wrap: max position diff %.2e\n", worst);
    CHECK(worst < 1e-3F);
}

void testPoseBlender(const fs::path &assets) {
    const auto model = mmd::pmx::load(assets / L"テスト用モデル" / L"物理テストキャラ.pmx");
    const auto idle = mmd::loadVmd(assets / L"モーション" / L"待機テスト.vmd");
    const auto sit = mmd::loadVmd(assets / L"モーション" / L"つまみ.vmd");
    mmd::MmdAnimator a(model), b(model);
    a.setMotion(&idle);
    b.setMotion(&sit);
    mmd::AnimatedModelFrame from, to;
    a.evaluateInto(from, 30.0F, 0.0F, true, {}, mmd::VertexOutput::omitUnchangedGpu);
    b.evaluateInto(to, 18.0F, 0.0F, true, {}, mmd::VertexOutput::omitUnchangedGpu);

    PoseBlender blender(model);
    std::vector<BoneTransform> out;
    const auto maxJointDistance = [&](const std::vector<BoneTransform> &x, const std::vector<BoneTransform> &y) {
        float worst = 0.0F;
        for (std::size_t i = 0; i < model.bones.size(); ++i)
            worst = std::max(worst, distance(jointPosition(model, x, i), jointPosition(model, y, i)));
        return worst;
    };
    blender.blend(from.bones, to.bones, 0.0F, out);
    CHECK(maxJointDistance(out, from.bones) < 1e-4F);
    blender.blend(from.bones, to.bones, 1.0F, out);
    CHECK(maxJointDistance(out, to.bones) < 1e-4F);
    blender.blend(from.bones, from.bones, 0.37F, out);
    CHECK(maxJointDistance(out, from.bones) < 1e-4F);

    // Half way: every bone keeps its distance to the parent (limbs do not shrink),
    // which a plain interpolation of joint positions does not guarantee.
    blender.blend(from.bones, to.bones, 0.5F, out);
    float worstLength = 0.0F;
    float moved = 0.0F;
    for (std::size_t i = 0; i < model.bones.size(); ++i) {
        const auto parent = model.bones[i].parent;
        if (parent < 0)
            continue;
        const auto p = static_cast<std::size_t>(parent);
        const float bind = distance(model.bones[i].position, model.bones[p].position);
        const float blended = distance(jointPosition(model, out, i), jointPosition(model, out, p));
        const float fromLength = distance(jointPosition(model, from.bones, i), jointPosition(model, from.bones, p));
        const float toLength = distance(jointPosition(model, to.bones, i), jointPosition(model, to.bones, p));
        // Poses that keep bind length keep it while blending.
        if (std::abs(fromLength - bind) < 1e-3F && std::abs(toLength - bind) < 1e-3F)
            worstLength = std::max(worstLength, std::abs(blended - bind));
        moved = std::max(moved, distance(jointPosition(model, out, i), jointPosition(model, from.bones, i)));
    }
    std::printf("    half-way blend: max bone length error %.2e, max joint travel %.2f\n", worstLength, moved);
    CHECK(worstLength < 1e-3F);
    CHECK(moved > 0.05F);

    std::vector<BoneTransform> lerped;
    lerpBones(from.bones, to.bones, 0.0F, lerped);
    CHECK(maxJointDistance(lerped, from.bones) < 1e-5F);
    lerpBones(from.bones, to.bones, 1.0F, lerped);
    CHECK(maxJointDistance(lerped, to.bones) < 1e-5F);
}

void testSeatAnchor(const fs::path &assets) {
    const auto model = mmd::pmx::load(assets / L"テスト用モデル" / L"テストキャラ.pmx");
    std::vector<BoneTransform> rest(model.bones.size());

    // The synthetic model has no pelvis/thigh bones: bone mode on センター (joint height 8).
    SeatAnchor anchor;
    anchor.bind(model, {});
    CHECK(std::string(anchor.mode()) == "bone" && anchor.contactPointCount() == 0);
    CHECK(std::abs(anchor.evaluate(model, rest)[1] - 8.0F) < 1e-4F);

    // Contact bones chosen by the author: the legs hang from センター and reach y = 0.
    SeatSettings settings;
    settings.contactBones = {"センター"};
    anchor.bind(model, settings);
    CHECK(std::string(anchor.mode()) == "contact" && anchor.contactPointCount() >= 8);
    auto point = anchor.evaluate(model, rest);
    CHECK(std::abs(point[1]) < 1e-3F);
    CHECK(std::abs(point[0]) < 1e-3F);

    // The lowest point follows the pose: lift the bone by 2 and tilt it.
    rest[0].translation = {0.0F, 2.0F, 0.0F};
    CHECK(std::abs(anchor.evaluate(model, rest)[1] - 2.0F) < 1e-3F);
    const float half = std::sin(0.25F), cosHalf = std::cos(0.25F); // 0.5 rad around Z through the origin
    rest[0].rotation = {0.0F, 0.0F, half, cosHalf};
    rest[0].translation = {};
    float expected = 1e9F;
    for (const auto &vertex : model.vertices)
        if (vertex.bones[0] == 0)
            expected = std::min(expected, vertex.position[0] * std::sin(0.5F) + vertex.position[1] * std::cos(0.5F));
    CHECK(std::abs(anchor.evaluate(model, rest)[1] - expected) < 0.02F);

    // Offsets and the joint-only mode.
    settings.offsetX = 0.5F;
    settings.offsetY = -0.25F;
    anchor.bind(model, settings);
    rest[0] = {};
    point = anchor.evaluate(model, rest);
    CHECK(std::abs(point[0] + 0.5F) < 1e-3F && std::abs(point[1] - 0.25F) < 1e-3F);
    settings = {};
    settings.useContact = false;
    settings.anchorBone = "頭";
    anchor.bind(model, settings);
    CHECK(std::string(anchor.mode()) == "bone" && anchor.anchorName() == "頭");
    CHECK(std::abs(anchor.evaluate(model, rest)[1] - 15.0F) < 1e-4F);
}

void testCameraOffset() {
    Camera camera;
    camera.target = {0.0F, 10.0F, 0.0F};
    camera.distance = 40.0F;
    camera.yaw = 0.3F;
    const float width = 384.0F, height = 682.0F;
    const mmd::Float3 point{1.5F, 12.0F, -0.5F};
    const auto base = camera.project(point, width, height);
    // Shift the image 10.25 px right and 6.5 px down.
    camera.ndcOffsetX = 2.0F * 10.25F / width;
    camera.ndcOffsetY = -2.0F * 6.5F / height;
    const auto shifted = camera.project(point, width, height);
    CHECK(std::abs(shifted.x - base.x - 10.25F) < 0.01F);
    CHECK(std::abs(shifted.y - base.y - 6.5F) < 0.01F);
    // Picking stays consistent: the ray through the shifted pixel passes the point.
    const auto ray = camera.ray(shifted.x, shifted.y, width, height);
    const mmd::Float3 delta{point[0] - ray.origin.x, point[1] - ray.origin.y, point[2] - ray.origin.z};
    const float along = delta[0] * ray.direction.x + delta[1] * ray.direction.y + delta[2] * ray.direction.z;
    const mmd::Float3 closest{ray.origin.x + ray.direction.x * along, ray.origin.y + ray.direction.y * along,
                              ray.origin.z + ray.direction.z * along};
    CHECK(distance(closest, point) < 0.01F);
}

void testExtraModel(const fs::path &modelPath, const fs::path &motionPath) {
    const auto model = mmd::pmx::load(modelPath);
    const auto motion = mmd::loadVmd(motionPath);
    std::vector<mmd::MorphOverride> overrides;
    for (std::size_t i = 0; i < model.morphs.size() && overrides.size() < 3; ++i)
        if (model.morphs[i].type == 1 && !model.morphs[i].offsets.empty())
            overrides.push_back({i, 0.8F, 1, {}});
    std::size_t types[5]{};
    for (const auto &v : model.vertices)
        ++types[std::min<std::size_t>(static_cast<std::size_t>(v.weightType), 4)];
    std::printf("    extra model weights: BDEF1 %zu, BDEF2 %zu, BDEF4 %zu, SDEF %zu, QDEF %zu\n", types[0], types[1],
                types[2], types[3], types[4]);
    const auto d = equivalence(model, &motion, static_cast<float>(motion.lastFrame), overrides);
    std::printf("    extra model (%zu vertices, %zu morph overrides): max position diff %.2e, normal diff %.2e\n",
                model.vertices.size(), overrides.size(), d.position, d.normal);
    CHECK(d.position < 5e-4F && d.normal < 5e-4F);

    // Real models rarely use SDEF / QDEF. Turn the two-bone vertices of the
    // real mesh into SDEF (C on the bone segment, as PMX editors place it) and
    // every third one into QDEF, and compare again.
    auto converted = model;
    std::size_t sdef = 0, qdef = 0;
    for (std::size_t i = 0; i < converted.vertices.size(); ++i) {
        auto &v = converted.vertices[i];
        if (v.weightType != mmd::PmxWeightType::bdef2 || v.bones[0] < 0 || v.bones[1] < 0 ||
            v.bones[0] == v.bones[1])
            continue;
        if (i % 3 == 0) {
            v.weightType = mmd::PmxWeightType::qdef;
            v.bones[2] = v.bones[3] = -1;
            v.weights = {v.weights[0], 1.0F - v.weights[0], 0.0F, 0.0F};
            ++qdef;
            continue;
        }
        const auto &a = converted.bones[static_cast<std::size_t>(v.bones[0])].position;
        const auto &b = converted.bones[static_cast<std::size_t>(v.bones[1])].position;
        const mmd::Float3 axis{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float lengthSq = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
        float t = lengthSq > 1e-8F ? ((v.position[0] - a[0]) * axis[0] + (v.position[1] - a[1]) * axis[1] +
                                      (v.position[2] - a[2]) * axis[2]) / lengthSq
                                   : 0.0F;
        t = std::clamp(t, 0.0F, 1.0F);
        v.weightType = mmd::PmxWeightType::sdef;
        v.sdefC = {a[0] + axis[0] * t, a[1] + axis[1] * t, a[2] + axis[2] * t};
        v.sdefR0 = v.sdefC;
        v.sdefR1 = v.sdefC;
        ++sdef;
    }
    const auto dc = equivalence(converted, &motion, static_cast<float>(motion.lastFrame), overrides);
    std::printf("    extra model as SDEF %zu / QDEF %zu: max position diff %.2e, normal diff %.2e\n", sdef, qdef,
                dc.position, dc.normal);
    CHECK(sdef > 0 && qdef > 0);
    CHECK(dc.position < 5e-4F && dc.normal < 5e-4F);

    SeatAnchor anchor;
    anchor.bind(model, {});
    std::string bones;
    for (const auto &name : anchor.contactBoneNames())
        bones += name + " ";
    std::vector<BoneTransform> rest(model.bones.size());
    const auto point = anchor.evaluate(model, rest);
    std::printf("    seat anchor: mode=%s anchor=%s bones=[%s] points=%zu rest=(%.2f, %.2f, %.2f)\n", anchor.mode(),
                anchor.anchorName().c_str(), bones.c_str(), anchor.contactPointCount(), point[0], point[1], point[2]);
}

// Minimal legged character: hips, legs with knees and ankles, arms, a head and
// one box per foot. Built here so the test does not depend on generated assets.
mmd::PmxModel leggedModel(bool withAnkles) {
    mmd::PmxModel model;
    const auto bone = [&](const char *name, mmd::Float3 position, int parent) {
        mmd::PmxBone b;
        b.name = name;
        b.position = position;
        b.parent = parent;
        b.flags = 0x001E; // rotatable, movable, visible, operable
        model.bones.push_back(b);
        return static_cast<int>(model.bones.size() - 1);
    };
    const int center = bone("センター", {0.0F, 8.5F, 0.0F}, -1);
    const int upper = bone("上半身", {0.0F, 10.0F, 0.0F}, center);
    const int head = bone("頭", {0.0F, 15.0F, 0.0F}, upper);
    bone("左ひじ", {4.0F, 12.5F, 0.0F}, bone("左腕", {1.5F, 14.0F, 0.0F}, upper));
    bone("右ひじ", {-4.0F, 12.5F, 0.0F}, bone("右腕", {-1.5F, 14.0F, 0.0F}, upper));
    std::vector<int> ankles;
    for (const auto &[side, x] : {std::pair{"左", 1.0F}, std::pair{"右", -1.0F}}) {
        const int thigh = bone((std::string(side) + "足").c_str(), {x, 8.5F, 0.0F}, center);
        const int knee = bone((std::string(side) + "ひざ").c_str(), {x, 4.7F, -0.1F}, thigh);
        ankles.push_back(withAnkles ? bone((std::string(side) + "足首").c_str(), {x, 1.0F, 0.0F}, knee) : knee);
    }
    const auto vertex = [&](mmd::Float3 position, int boneIndex) {
        mmd::PmxVertex v{};
        v.position = position;
        v.normal = {0.0F, 1.0F, 0.0F};
        v.weightType = mmd::PmxWeightType::bdef1;
        v.bones = {boneIndex, -1, -1, -1};
        v.weights = {1.0F, 0.0F, 0.0F, 0.0F};
        model.vertices.push_back(v);
    };
    for (std::size_t i = 0; i < ankles.size(); ++i) {
        const float x = i == 0 ? 1.0F : -1.0F;
        for (const float dx : {-0.4F, 0.4F})
            for (const float z : {-1.6F, 0.4F})
                for (const float y : {0.0F, 0.6F})
                    vertex({x + dx, y, z}, ankles[i]);
    }
    vertex({0.0F, 16.0F, 0.0F}, head);
    vertex({0.0F, 9.0F, 0.0F}, center);
    return model;
}

void testMotionGenerator() {
    const auto model = leggedModel(true);
    const auto rig = motion::analyze(model);
    CHECK(rig.valid);
    CHECK(std::abs(rig.legLength - 7.5F) < 0.05F);
    CHECK(std::abs(rig.ground) < 1e-4F);
    CHECK(rig.hasArms && rig.hasElbows && !rig.hasLegIk);

    // Walk: the planted foot keeps pace with the body, nothing sinks into the floor.
    const float speed = 5.0F;
    const auto params = motion::fitWalk(rig, speed);
    motion::WalkInfo info;
    const auto walk = motion::makeWalk(rig, params, info);
    std::printf("    walk: %u frames, %.2f units/s (asked %.2f)\n", params.frames, info.speed, speed);
    CHECK(std::abs(info.speed - speed) < 0.02F * speed);
    const auto w = motion::measureContact(model, rig, walk, info.speed);
    std::printf("    walk: slide mean %.2f%% max %.1f%%, %d planted samples, below floor %.4f\n",
                100.0F * w.slideMean / info.speed, 100.0F * w.slideMax / info.speed, w.samples, w.penetration);
    CHECK(w.samples > 40);
    CHECK(w.slideMean < 0.03F * info.speed);
    CHECK(w.penetration < 0.01F * rig.legLength);

    // Idle: the feet do not move or sink.
    const auto idle = motion::measureContact(model, rig, motion::makeIdle(rig), 0.0F);
    std::printf("    idle: drift %.4f, below floor %.4f\n", idle.drift, idle.penetration);
    CHECK(idle.drift < 0.01F * rig.legLength);
    CHECK(idle.penetration < 0.01F * rig.legLength);

    // Sit: thighs level, shins hanging.
    const auto sit = motion::makeSit(rig);
    mmd::MmdAnimator animator(model);
    animator.setMotion(&sit);
    mmd::AnimatedModelFrame frame;
    animator.evaluateInto(frame, 0.0F, 0.0F, true, {}, mmd::VertexOutput::omitUnchangedGpu);
    const auto hip = jointPosition(model, frame.bones, 7);
    const auto knee = jointPosition(model, frame.bones, 8);
    const auto ankle = jointPosition(model, frame.bones, 9);
    std::printf("    sit: hip y %.2f knee (%.2f, z %.2f) ankle y %.2f\n", hip[1], knee[1], knee[2], ankle[1]);
    CHECK(std::abs(knee[1] - hip[1]) < 0.15F * rig.left.thighLength);
    CHECK(knee[2] < hip[2] - 0.9F * rig.left.thighLength); // in front of the hips (-Z)
    CHECK(ankle[1] < knee[1] - 0.9F * rig.left.shinLength);

    // Every generated motion only uses bones the model has.
    for (const auto &m : {walk, motion::makeIdle(rig), sit, motion::makeGrab(rig)}) {
        bool known = true;
        for (const auto &key : m.bones)
            known = known && std::ranges::any_of(model.bones, [&](const mmd::PmxBone &b) { return b.name == key.name; });
        CHECK(known);
        CHECK(m.lastFrame > 0);
    }

    // Without ankles the model cannot be fitted.
    CHECK(!motion::analyze(leggedModel(false)).valid);
}

void run(const char *name, const std::function<void()> &test) {
    std::printf("[%s]\n", name);
    const int before = g_failures;
    try {
        test();
    } catch (const std::exception &e) {
        ++g_failures;
        std::printf("  FAILED: exception: %s\n", e.what());
    }
    std::printf("  %s\n", g_failures == before ? "ok" : "NG");
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        std::printf("usage: uka3d_quality_tests <test-assets-dir> [<model.pmx> <motion.vmd>]\n");
        return 2;
    }
    const fs::path assets = argv[1];
    run("skinning_equivalence", [&] { testSkinningEquivalence(assets); });
    run("pose_only_with_physics", [&] { testPoseOnlyWithPhysics(assets); });
    run("pose_blender", [&] { testPoseBlender(assets); });
    run("seat_anchor", [&] { testSeatAnchor(assets); });
    run("camera_offset", testCameraOffset);
    run("motion_generator", testMotionGenerator);
    if (argc >= 4)
        run("extra_model", [&] { testExtraModel(argv[2], argv[3]); });
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
