#include "mmd/MmdModel.h"

#include <DirectXMath.h>

#include "platform/Log.h"
#include "platform/Utf.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace uka3d {

std::optional<std::size_t> MmdModel::findBone(std::string_view name) const {
    if (name.empty())
        return std::nullopt;
    for (std::size_t i = 0; i < model_->bones.size(); ++i) {
        const auto &bone = model_->bones[i];
        if (bone.name == name || bone.englishName == name)
            return i;
    }
    return std::nullopt;
}

mmd::Float3 MmdModel::bonePosition(std::size_t index, bool posed) const {
    const auto &position = model_->bones[index].position;
    return posed ? transformBoneOffset(index, {}) : position;
}

mmd::Float3 MmdModel::transformBoneOffset(std::size_t index, const mmd::Float3 &offset) const {
    auto position = model_->bones[index].position;
    for (std::size_t axis = 0; axis < position.size(); ++axis)
        position[axis] += offset[axis];
    if (index >= frame_.bones.size())
        return position;
    // libmmd supplies the skinning transform, so transform the bind position
    // to recover the animated joint in model coordinates.
    const auto &bone = frame_.bones[index];
    using namespace DirectX;
    const auto rotation = XMVectorSet(bone.rotation[0], bone.rotation[1], bone.rotation[2], bone.rotation[3]);
    const auto joint = XMVector3Rotate(XMVectorSet(position[0], position[1], position[2], 0.0F), rotation);
    return {XMVectorGetX(joint) + bone.translation[0], XMVectorGetY(joint) + bone.translation[1],
            XMVectorGetZ(joint) + bone.translation[2]};
}
namespace {

void requireRegularFile(const std::filesystem::path &path, std::string_view kind) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        throw LoadError(std::format("{}ファイルが見つかりません: {}", kind, pathToUtf8(path)));
    if (!std::filesystem::is_regular_file(path, ec))
        throw LoadError(std::format("{}のパスが通常ファイルではありません: {}", kind, pathToUtf8(path)));
}

} // namespace

void MmdModel::loadModel(const std::filesystem::path &inputPath) {
    const auto path = std::filesystem::absolute(inputPath).lexically_normal();
    requireRegularFile(path, "PMXモデル");
    log::info("PMX読み込み開始: {}", pathToUtf8(path));

    auto model = std::make_unique<mmd::PmxModel>();
    try {
        *model = mmd::pmx::load(path);
    } catch (const std::exception &e) {
        throw LoadError(std::format("PMXの読み込みに失敗しました（ファイル破損または非対応形式の可能性）: {}\n  詳細: {}",
                                    pathToUtf8(path), e.what()));
    }
    if (model->vertices.empty() || model->indices.empty())
        throw LoadError(std::format("PMXに描画可能なメッシュがありません: {}", pathToUtf8(path)));

    // libmmd's validator is strict (e.g. dangling display-frame entries). Many
    // distributed models trip it while still rendering correctly, so issues are
    // reported but only render-critical problems (below) reject the model.
    const auto validation = mmd::pmx::validate(*model);
    std::size_t reported = 0;
    for (const auto &issue : validation.issues) {
        if (reported++ >= 20) {
            log::warn("PMX検証: ほか {} 件省略", validation.issues.size() - 20);
            break;
        }
        log::warn("PMX検証: {} {} (index {})", issue.object, issue.message, issue.location.subIndex);
    }

    std::uint64_t materialIndexTotal = 0;
    for (const auto &material : model->materials)
        materialIndexTotal += material.indexCount;
    if (materialIndexTotal > model->indices.size())
        throw LoadError(std::format("PMXのマテリアル面数が総インデックス数を超えています（破損の可能性）: {}",
                                    pathToUtf8(path)));
    for (const auto index : model->indices) {
        if (index >= model->vertices.size())
            throw LoadError(std::format("PMXの頂点インデックスが範囲外です（破損の可能性）: {}", pathToUtf8(path)));
    }

    const auto &meta = model->metadata;
    log::info("PMX読み込み成功: 名前=\"{}\" バージョン={:.1f} 頂点={} 面={} マテリアル={} テクスチャ={} ボーン={} "
              "モーフ={} 剛体={}",
              meta.modelName, meta.version, model->vertices.size(), model->indices.size() / 3,
              model->materials.size(), model->textures.size(), model->bones.size(), model->morphs.size(),
              model->rigidBodies.size());
    if (!meta.comment.empty())
        log::debug("PMXコメント: {}", meta.comment);

    animator_.reset();
    physics_.reset();
    motion_.reset();
    physicsStatus_ = PhysicsStatus::notInitialized;
    physicsEnabled_ = false;
    overrides_.clear();
    motionPath_.clear();
    modelPath_ = path;
    model_ = std::move(model);
    skinner_.reset();
    if (model_->softBodies.empty())
        skinner_ = std::make_unique<Skinner>(*model_);
    animator_ = std::make_unique<mmd::MmdAnimator>(*model_);
}

void MmdModel::loadMotion(const std::filesystem::path &inputPath) {
    if (!model_)
        throw LoadError("モーションより先にモデルを読み込む必要があります");
    const auto path = std::filesystem::absolute(inputPath).lexically_normal();
    requireRegularFile(path, "VMDモーション");
    log::info("VMD読み込み開始: {}", pathToUtf8(path));

    auto motion = std::make_unique<mmd::VmdMotion>();
    try {
        *motion = mmd::loadVmd(path);
    } catch (const std::exception &e) {
        throw LoadError(std::format("VMDの読み込みに失敗しました（ファイル破損または非対応形式の可能性）: {}\n  詳細: {}",
                                    pathToUtf8(path), e.what()));
    }

    animator_->setMotion(motion.get());
    motion_ = std::move(motion);
    motionPath_ = path;
    const auto compat = animator_->motionCompatibility();
    log::info("VMD読み込み成功: 対象モデル名=\"{}\" 最終フレーム={} ボーンキー={} モーフキー={} "
              "一致ボーントラック={}/{}",
              motion_->modelName, motion_->lastFrame, motion_->bones.size(), motion_->morphs.size(),
              compat.matchedBoneTrackCount, compat.vmdBoneTrackCount);
    if (compat.vmdBoneTrackCount > 0 && compat.matchedBoneTrackCount == 0)
        log::warn("VMDのボーン名がモデルと一致しません。モデルは動きません");
}

void MmdModel::clearMotion() {
    if (animator_)
        animator_->setMotion(nullptr);
    motion_.reset();
    motionPath_.clear();
}

bool MmdModel::setMorphWeight(const std::string &name, float weight) {
    if (!model_)
        return false;
    const auto &morphs = model_->morphs;
    const auto found = std::ranges::find_if(morphs, [&](const auto &morph) { return morph.name == name; });
    if (found == morphs.end())
        return false;
    const auto index = static_cast<std::size_t>(found - morphs.begin());
    weight = std::clamp(std::isfinite(weight) ? weight : 0.0F, 0.0F, 1.0F);
    for (auto &entry : overrides_) {
        if (entry.index == index) {
            entry.weight = weight;
            return true;
        }
    }
    mmd::MorphOverride entry;
    entry.index = index;
    entry.weight = weight;
    entry.type = found->type;
    overrides_.push_back(entry);
    return true;
}

bool MmdModel::hasMorph(const std::string &name) const {
    return model_ && std::ranges::any_of(model_->morphs, [&](const auto &morph) { return morph.name == name; });
}

bool MmdModel::resetMorph(const std::string &name) {
    if (name.empty()) {
        const bool any = !overrides_.empty();
        overrides_.clear();
        return any;
    }
    const auto before = overrides_.size();
    std::erase_if(overrides_, [&](const auto &entry) {
        return entry.index < model_->morphs.size() && model_->morphs[entry.index].name == name;
    });
    return overrides_.size() != before;
}

std::vector<std::pair<std::string, float>> MmdModel::morphOverrides() const {
    std::vector<std::pair<std::string, float>> result;
    for (const auto &entry : overrides_)
        if (entry.index < model_->morphs.size())
            result.emplace_back(model_->morphs[entry.index].name, entry.weight);
    return result;
}

float MmdModel::motionLastFrame() const noexcept {
    return motion_ ? static_cast<float>(motion_->lastFrame) : 0.0F;
}

void MmdModel::run(float frame, float seconds, bool poseOnly) {
    // Full: libmmd returns CPU-skinned vertices (morphs, SDEF, QDEF). With physics
    // attached, libmmd runs: animation -> IK -> kinematic bodies -> Bullet step
    // -> physics bones -> post-physics IK -> skinning, all inside this call.
    // Pose only: the same pipeline without the vertex copy and skinning.
    animator_->evaluateInto(frame_, frame, seconds, poseOnly, overrides_,
                            poseOnly ? mmd::VertexOutput::omitUnchangedGpu : mmd::VertexOutput::full);
}

const mmd::AnimatedModelFrame &MmdModel::evaluate(float frame, float seconds) {
    run(frame, seconds, false);
    return frame_;
}

const mmd::AnimatedModelFrame &MmdModel::evaluatePose(float frame, float seconds) {
    run(frame, seconds, poseOnlySupported());
    return frame_;
}

const mmd::AnimatedModelFrame &MmdModel::evaluateAcrossLoop(float frame, float seconds) {
    return acrossLoop(frame, seconds, false);
}

const mmd::AnimatedModelFrame &MmdModel::evaluatePoseAcrossLoop(float frame, float seconds) {
    return acrossLoop(frame, seconds, poseOnlySupported());
}

const mmd::AnimatedModelFrame &MmdModel::acrossLoop(float frame, float seconds, bool poseOnly) {
    if (!physicsEnabled_ || physics_ == nullptr) {
        run(frame, seconds, poseOnly);
        return frame_;
    }
    // 1) Remember where every simulated body is.
    const std::size_t count = physics_->bodyCount();
    savedBodies_.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        savedBodies_[i] = physics_->bodyTransform(i);
    // 2) Let libmmd process the backward jump without stepping (seconds = 0):
    //    it resets the world and snaps every body to the frame's animated pose.
    //    Only the physics side effect matters here, so skip the vertices.
    run(frame, 0.0F, poseOnlySupported());
    // 3) Put the dynamic bodies back. Kinematic (mode 0) bodies keep the new
    //    pose. Velocities are not restorable through the public API, so the
    //    bodies resume from rest at their previous positions.
    for (std::size_t i = 0; i < count; ++i) {
        if (physics_->bodyMode(i) != 0)
            physics_->teleportBody(i, savedBodies_[i]);
    }
    // 4) Same frame again with the real step: libmmd sees frameDelta == 0,
    //    treats it as continuous and simulates `seconds`.
    run(frame, seconds, poseOnly);
    return frame_;
}

const char *toString(PhysicsStatus status) {
    switch (status) {
    case PhysicsStatus::notInitialized:
        return "未初期化";
    case PhysicsStatus::disabledByUser:
        return "無効（--no-physics）";
    case PhysicsStatus::noRigidBodies:
        return "剛体なし";
    case PhysicsStatus::unavailable:
        return "利用不可（物理エンジンなしでビルド）";
    case PhysicsStatus::failed:
        return "初期化失敗";
    case PhysicsStatus::active:
        return "有効";
    }
    return "?";
}

PhysicsStatus MmdModel::setupPhysics(const PhysicsOptions &options) {
    if (!model_)
        return physicsStatus_;
    animator_->setPhysics(nullptr);
    physics_.reset();
    physicsEnabled_ = false;

    const auto &pmx = *model_;
    if (pmx.rigidBodies.empty()) {
        physicsStatus_ = PhysicsStatus::noRigidBodies;
        log::info("物理演算: モデルに剛体がないため使用しません");
        return physicsStatus_;
    }
    try {
        mmd::PhysicsSettings settings;
        // MMD (Bullet 2.75) compatible joints, as used by MMD-authored models.
        settings.compatibility = mmd::PhysicsCompatibilityProfile::mmd275Compatible();
        // 1/60 s base step. Slightly enlarged so ceil((n/60) / step) == n in
        // float arithmetic (libmmd sub-steps with ceil(dt / fixedTimeStep)).
        settings.fixedTimeStep = static_cast<float>(1.0 / 60.0) * 1.0001F;
        settings.collectContactDiagnostics = false; // not needed, saves time
        auto physics = std::make_unique<mmd::MmdPhysics>(pmx, settings);
        if (options.simulateMissingBackend || !physics->available()) {
            physicsStatus_ = PhysicsStatus::unavailable;
            log::warn("物理演算: 物理エンジン(Bullet)が利用できないため、物理なしで再生します");
            return physicsStatus_;
        }
        log::info("物理演算: 剛体={} (モデル {}) ジョイント={} (モデル {})", physics->bodyCount(),
                  pmx.rigidBodies.size(), physics->jointCount(), pmx.joints.size());
        physics_ = std::move(physics);
    } catch (const std::exception &e) {
        physicsStatus_ = PhysicsStatus::failed;
        log::warn("物理演算の初期化に失敗したため、物理なしで再生します: {}", e.what());
        return physicsStatus_;
    }
    physicsStatus_ = options.enabled ? PhysicsStatus::active : PhysicsStatus::disabledByUser;
    if (options.enabled)
        setPhysicsEnabled(true);
    else
        log::info("物理演算: --no-physics のため無効（メニューまたは P キーで有効化できます）");
    return physicsStatus_;
}

bool MmdModel::setPhysicsEnabled(bool enabled) {
    if (enabled && physics_ == nullptr)
        enabled = false;
    if (enabled == physicsEnabled_)
        return physicsEnabled_;
    physicsEnabled_ = enabled;
    // setPhysics() also makes the next evaluate() re-synchronise the world
    // with the animated pose (or drop the physics bones when detached).
    animator_->setPhysics(enabled ? physics_.get() : nullptr);
    if (enabled)
        physicsStatus_ = PhysicsStatus::active;
    log::info("物理演算: {}", enabled ? "ON" : "OFF");
    return physicsEnabled_;
}

void MmdModel::resetPhysics() {
    if (physicsEnabled_ && physics_ != nullptr)
        animator_->setPhysics(physics_.get()); // marks the next evaluation as the first one
}

Bounds MmdModel::computeBounds(const std::vector<mmd::PmxVertex> &vertices) {
    Bounds bounds;
    constexpr float inf = std::numeric_limits<float>::infinity();
    bounds.minimum = {inf, inf, inf};
    bounds.maximum = {-inf, -inf, -inf};
    for (const auto &vertex : vertices) {
        const auto &p = vertex.position;
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            continue;
        for (int axis = 0; axis < 3; ++axis) {
            bounds.minimum[axis] = std::min(bounds.minimum[axis], p[axis]);
            bounds.maximum[axis] = std::max(bounds.maximum[axis], p[axis]);
        }
        bounds.valid = true;
    }
    if (!bounds.valid)
        bounds = {};
    return bounds;
}

Bounds MmdModel::bindPoseBounds() const {
    return model_ ? computeBounds(model_->vertices) : Bounds{};
}

std::filesystem::path MmdModel::resolveTexturePath(std::size_t textureIndex) const {
    // libmmd's pmx::resolveTexturePath converts the UTF-8 name through the ANSI
    // code page on Windows, so resolve it here with explicit UTF-8 decoding.
    std::string stored = model_->textures.at(textureIndex).storedPath;
    std::ranges::replace(stored, '\\', '/');
    return (model_->sourcePath.parent_path() / utf8ToPath(stored)).lexically_normal();
}

} // namespace uka3d
