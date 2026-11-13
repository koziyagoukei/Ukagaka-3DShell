#pragma once

#include <windows.h>

#include <string>

namespace uka3d {

// Frame-rate limiter that never busy-waits.
//
// Two strategies, chosen from the monitor refresh rate so that we never wait
// twice (timer + VSync) for the same frame:
//
//  * vsync  : refresh is (close to) an integer multiple N of the target, N<=4.
//             Present(N) alone paces the frames (e.g. 120 Hz / 60 fps -> N=2).
//             Targets at or above the refresh rate use N=1 (capped by the display).
//  * timer  : otherwise (e.g. 144 Hz / 60 fps). Present(0) and a
//             high-resolution waitable timer sleep until the next deadline.
//             Deadlines are scheduled on an absolute grid, so jitter does not
//             accumulate.
//  * unlimited (target 0): Present(0), no waiting.
class FramePacer {
  public:
    enum class Mode { vsync, timer, unlimited };

    explicit FramePacer(int targetFps);
    ~FramePacer();
    FramePacer(const FramePacer &) = delete;
    FramePacer &operator=(const FramePacer &) = delete;

    // (Re)computes the strategy for the given refresh rate (Hz). Returns true when it changed.
    bool configure(double refreshHz);

    // Timer mode: true when the next frame is due. Other modes: always true.
    [[nodiscard]] bool frameDue() const;
    // Timer mode: blocks until the frame is due OR a window message arrives
    // (so input stays responsive). Other modes: returns immediately.
    void waitForFrameOrMessage() const;
    // Call right after presenting a frame.
    void framePresented();

    [[nodiscard]] UINT syncInterval() const noexcept {
        return syncInterval_;
    }
    [[nodiscard]] Mode mode() const noexcept {
        return mode_;
    }
    [[nodiscard]] int targetFps() const noexcept {
        return targetFps_;
    }
    [[nodiscard]] std::string describe() const;

    // Refresh rate of the monitor that shows `hwnd` (Hz, 0 if unknown).
    static double monitorRefreshRate(HWND hwnd);

  private:
    [[nodiscard]] long long now() const;

    int targetFps_;
    double refreshHz_{0.0};
    Mode mode_{Mode::vsync};
    UINT syncInterval_{1};
    HANDLE timer_{nullptr};
    bool highResolutionTimer_{false};
    long long frequency_{1};
    long long period_{0};       // QPC ticks per frame (timer mode)
    long long nextDeadline_{0}; // QPC
};

} // namespace uka3d
