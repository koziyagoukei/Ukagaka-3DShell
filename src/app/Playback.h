#pragma once

#include <cstdint>

namespace uka3d {

// Decides, per rendered frame, which motion frame to evaluate and how much
// simulation time to hand to libmmd. Pure logic (no Win32/D3D) so it can be
// unit-tested.
//
// * Physics on : fixed-step accumulation. Real time is accumulated and
//   consumed in whole ticks of 1/60 s; one evaluate() covers all ticks of a
//   render frame (libmmd sub-steps internally at the same 1/60 s). The
//   remainder is exposed as interpolationAlpha() so the renderer can blend
//   between the last two simulated states at render rates above 60 fps.
// * Physics off: variable step, the motion advances by the real elapsed time
//   every render frame (smoothest at any frame rate).
//
// In both modes the elapsed time per update is clamped so a stall (window
// drag, breakpoint, sleep) cannot make the simulation explode.
class Playback {
  public:
    static constexpr double kTickSeconds = 1.0 / 60.0;
    static constexpr int kMaxTicksPerUpdate = 5;         // at most 83 ms of catch-up
    static constexpr double kMaxVariableStep = 0.1;      // physics off: max 100 ms per update
    static constexpr double kMotionFps = 30.0;           // VMD frames per second

    enum class Kind : std::uint8_t {
        none,    // nothing to evaluate (paused, idle)
        advance, // continuous step: evaluate(frame, seconds)
        wrap,    // loop boundary crossed: evaluate preserving the physics state
        seek,    // discontinuous jump (start, rewind): evaluate(frame, 0) and reset physics
    };

    struct Step {
        Kind kind{Kind::none};
        float frame{};   // motion frame to evaluate
        float seconds{}; // simulation time for this evaluation
        int ticks{};     // fixed ticks consumed (physics on)
    };

    struct Config {
        bool hasMotion{false};
        float lastFrame{0.0F};
        bool loop{true};
        bool fixedStep{true}; // physics active
    };

    explicit Playback(const Config &config);

    // Advances by the real elapsed time and returns what to evaluate.
    Step update(double realSeconds);

    void setPaused(bool paused);
    [[nodiscard]] bool paused() const noexcept {
        return paused_;
    }
    // Next update() returns a seek to frame 0 (physics is re-initialised).
    void rewind();
    // Starts another motion (or none) from frame 0 WITHOUT resetting physics:
    // the next evaluated step is reported as a wrap so dynamic bodies keep
    // their state (hair does not snap when switching greet -> idle).
    void restart(const Config &config);
    // A non-looping motion has reached its last frame.
    [[nodiscard]] bool finished() const noexcept {
        return config_.hasMotion && !config_.loop && frame_ >= config_.lastFrame;
    }
    [[nodiscard]] const Config &config() const noexcept {
        return config_;
    }
    // Switch between fixed-step (physics) and variable-step modes. Forces a
    // seek to the current frame so the physics state is re-synchronised.
    void setFixedStep(bool fixedStep);
    [[nodiscard]] bool fixedStep() const noexcept {
        return config_.fixedStep;
    }

    [[nodiscard]] double frame() const noexcept {
        return frame_;
    }
    // Fraction (0..1) of a tick that has accumulated but not yet been simulated.
    [[nodiscard]] float interpolationAlpha() const noexcept;
    [[nodiscard]] std::uint64_t totalTicks() const noexcept {
        return totalTicks_;
    }
    [[nodiscard]] std::uint64_t loopCount() const noexcept {
        return loops_;
    }
    [[nodiscard]] double droppedSeconds() const noexcept {
        return dropped_;
    }

  private:
    Step advanceMotion(double seconds, int ticks);

    Config config_;
    double frame_{0.0};
    double accumulator_{0.0};
    bool paused_{false};
    bool pendingSeek_{true}; // first update initialises the pose/physics
    bool pendingRestart_{false};
    std::uint64_t totalTicks_{0};
    std::uint64_t loops_{0};
    double dropped_{0.0};
};

} // namespace uka3d
