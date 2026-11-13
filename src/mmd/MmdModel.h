#pragma once

#include "mmd/Skinning.h"

#include <mmd/animation.hpp>
#include <mmd/physics.hpp>
#include <mmd/pmx.hpp>
#include <mmd/vmd.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace uka3d {

// Thrown for user-facing load failures. what() is UTF-8.
class LoadError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

enum class PhysicsStatus {
    notInitialized, // setupPhysics() not called yet
    disabledByUser, // --no-physics at start-up (can still be enabled later)
    noRigidBodies,  // model has no rigid bodies: nothing to simulate
    unavailable,    // built without a physics backend (Bullet)
    failed,         // backend rejected the model data
    active,         // simulation object exists (see physicsEnabled())
};
const char *toString(PhysicsStatus status);

struct PhysicsOptions {
    bool enabled{true};
    // Test hook: behave as if libmmd had been built without Bullet.
    bool simulateMissingBackend{false};
};

struct Bounds {
    mmd::Float3 minimum{};
    mmd::Float3 maximum{};
    bool valid{false};
};

// Owns a PMX model, an optional VMD motion and the libmmd animator bound to them.
// The model/motion objects are heap-allocated so the animator's references stay valid.
class MmdModel {
  public:
    MmdModel() = default;
    MmdModel(const MmdModel &) = delete;
    MmdModel &operator=(const MmdModel &) = delete;

    void loadModel(const std::filesystem::path &path);  // throws LoadError
    void loadMotion(const std::filesystem::path &path); // throws LoadError
    void clearMotion();
    [[nodiscard]] const std::filesystem::path &motionPath() const noexcept {
        return motionPath_;
    }
    [[nodiscard]] const std::filesystem::path &modelPath() const noexcept {
        return modelPath_;
    }

    // ---- Morph overrides (expression control) --------------------------------
    // Replaces the motion's weight for the named morph. Returns false if the
    // model has no such morph.
    bool setMorphWeight(const std::string &name, float weight);
    [[nodiscard]] bool hasMorph(const std::string &name) const;
    // Removes one override (true if it existed) or, with an empty name, all of them.
    bool resetMorph(const std::string &name);
    [[nodiscard]] std::vector<std::pair<std::string, float>> morphOverrides() const;

    // ---- Automatic face (blink, gaze) -------------------------------------------
    [[nodiscard]] std::optional<std::size_t> findMorph(std::string_view name) const;
    // The weight the current motion gives a morph at `frame` (VMD morph keys are
    // linear); 0 without a motion or keys.
    [[nodiscard]] float motionMorphWeight(std::size_t morph, float frame) const;
    // The expression override for a morph, if there is one.
    [[nodiscard]] std::optional<float> overrideWeight(std::size_t morph) const;
    // Added on every evaluation: an automatic morph weight (the blink), used as
    // the larger of it and the motion's weight unless an expression overrides
    // that morph, and gaze rotations multiplied onto bones before IK and physics.
    void setAutoMorph(std::optional<std::size_t> morph, float weight);
    void setGazeRotations(const std::vector<std::pair<std::size_t, mmd::Float4>> &rotations);

    [[nodiscard]] bool loaded() const noexcept {
        return model_ != nullptr;
    }
    [[nodiscard]] const mmd::PmxModel &model() const {
        return *model_;
    }
    [[nodiscard]] bool hasMotion() const noexcept {
        return motion_ != nullptr;
    }
    // Last keyframe of the motion (30 fps frames). 0 when no motion is loaded.
    [[nodiscard]] float motionLastFrame() const noexcept;

    // ---- Physics ----------------------------------------------------------
    // Creates the libmmd/Bullet world for the loaded model. Never throws:
    // problems are logged and reported through physicsStatus().
    PhysicsStatus setupPhysics(const PhysicsOptions &options);
    [[nodiscard]] PhysicsStatus physicsStatus() const noexcept {
        return physicsStatus_;
    }
    // True when a working simulation exists (it may be switched off).
    [[nodiscard]] bool physicsAvailable() const noexcept {
        return physics_ != nullptr;
    }
    [[nodiscard]] bool physicsEnabled() const noexcept {
        return physicsEnabled_;
    }
    // Attaches/detaches the simulation. Returns the resulting state.
    bool setPhysicsEnabled(bool enabled);
    // Forces the physics state to be re-initialised from the animated pose
    // on the next evaluate() (used on rewind).
    void resetPhysics();

    // Evaluates bones/morphs/IK/physics and CPU skinning.
    // `seconds` is the simulation time step handed to libmmd (0 = no step).
    const mmd::AnimatedModelFrame &evaluate(float frame, float seconds = 0.0F);
    // Same as evaluate(), but for a motion loop wrap: libmmd treats a backward
    // frame jump as a seek and resets the physics world. This keeps the
    // dynamic rigid bodies where they were so hair/skirts continue smoothly.
    const mmd::AnimatedModelFrame &evaluateAcrossLoop(float frame, float seconds);

    // ---- Fast path -----------------------------------------------------------
    // Bones, effective morph weights and materials only: libmmd neither copies
    // nor skins the vertices. Deform them with skinner(). Not available for
    // models with soft bodies (those need libmmd's vertex path).
    [[nodiscard]] bool poseOnlySupported() const noexcept {
        return skinner_ != nullptr;
    }
    const mmd::AnimatedModelFrame &evaluatePose(float frame, float seconds = 0.0F);
    const mmd::AnimatedModelFrame &evaluatePoseAcrossLoop(float frame, float seconds);
    [[nodiscard]] Skinner &skinner() {
        return *skinner_;
    }
    // Bone transforms of the most recent evaluation.
    [[nodiscard]] const std::vector<BoneTransform> &bones() const noexcept {
        return frame_.bones;
    }

    [[nodiscard]] Bounds bindPoseBounds() const;
    [[nodiscard]] std::optional<std::size_t> findBone(std::string_view name) const;
    [[nodiscard]] mmd::Float3 bonePosition(std::size_t index, bool posed) const;
    [[nodiscard]] mmd::Float3 transformBoneOffset(std::size_t index, const mmd::Float3 &offset) const;
    [[nodiscard]] static Bounds computeBounds(const std::vector<mmd::PmxVertex> &vertices);

    // Resolves a PMX texture entry relative to the model file (UTF-8 aware).
    [[nodiscard]] std::filesystem::path resolveTexturePath(std::size_t textureIndex) const;

  private:
    std::unique_ptr<mmd::PmxModel> model_;
    std::unique_ptr<mmd::VmdMotion> motion_;
    // Declared before animator_ so the animator (which references it) is destroyed first.
    std::unique_ptr<mmd::MmdPhysics> physics_;
    std::unique_ptr<mmd::MmdAnimator> animator_;
    PhysicsStatus physicsStatus_{PhysicsStatus::notInitialized};
    bool physicsEnabled_{false};
    std::vector<mmd::PhysicsTransform> savedBodies_;
    std::vector<mmd::MorphOverride> overrides_;
    std::vector<std::vector<std::pair<std::uint32_t, float>>> motionMorphKeys_; // per model morph
    std::optional<std::size_t> autoMorph_;
    float autoMorphWeight_{0.0F};
    float autoMorphFrame_{0.0F};
    std::vector<mmd::PmxMorphOffset> gazeOffsets_;
    std::vector<mmd::MorphOverride> combined_;
    std::filesystem::path modelPath_;
    std::filesystem::path motionPath_;
    mmd::AnimatedModelFrame frame_;
    std::unique_ptr<Skinner> skinner_;
    void run(float frame, float seconds, bool poseOnly);
    const mmd::AnimatedModelFrame &acrossLoop(float frame, float seconds, bool poseOnly);
};

} // namespace uka3d
