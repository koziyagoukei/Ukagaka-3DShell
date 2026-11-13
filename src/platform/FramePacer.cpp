#include "platform/FramePacer.h"

#include "platform/Log.h"

#include <timeapi.h>

#include <cmath>
#include <format>

namespace uka3d {

FramePacer::FramePacer(int targetFps) : targetFps_(targetFps) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    frequency_ = frequency.QuadPart;
    // High-resolution timers (Windows 10 1803+) give ~0.5 ms accuracy without
    // raising the global timer resolution.
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    highResolutionTimer_ = timer_ != nullptr;
    if (timer_ == nullptr) {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        timeBeginPeriod(1);
    }
    if (targetFps_ > 0)
        period_ = frequency_ / targetFps_;
}

FramePacer::~FramePacer() {
    if (timer_ != nullptr)
        CloseHandle(timer_);
    if (!highResolutionTimer_)
        timeEndPeriod(1);
}

long long FramePacer::now() const {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

bool FramePacer::configure(double refreshHz) {
    const Mode oldMode = mode_;
    const UINT oldInterval = syncInterval_;
    refreshHz_ = refreshHz;
    if (targetFps_ <= 0) {
        mode_ = Mode::unlimited;
        syncInterval_ = 0;
    } else if (refreshHz <= 0.0) {
        mode_ = Mode::timer; // unknown refresh rate: rely on the timer only
        syncInterval_ = 0;
    } else if (targetFps_ >= refreshHz - 1.0) {
        mode_ = Mode::vsync; // the display caps us anyway
        syncInterval_ = 1;
    } else {
        const double ratio = refreshHz / targetFps_;
        const long divisor = std::lround(ratio);
        const double achieved = refreshHz / static_cast<double>(divisor);
        if (divisor >= 1 && divisor <= 4 && std::abs(achieved - targetFps_) <= targetFps_ * 0.03) {
            mode_ = Mode::vsync;
            syncInterval_ = static_cast<UINT>(divisor);
        } else {
            mode_ = Mode::timer;
            syncInterval_ = 0;
        }
    }
    nextDeadline_ = now();
    return oldMode != mode_ || oldInterval != syncInterval_;
}

bool FramePacer::frameDue() const {
    if (mode_ != Mode::timer)
        return true;
    return now() >= nextDeadline_;
}

void FramePacer::waitForFrameOrMessage() const {
    if (mode_ != Mode::timer)
        return;
    const long long remaining = nextDeadline_ - now();
    if (remaining <= 0)
        return;
    // Relative due time in 100 ns units (negative = relative).
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(remaining * 10'000'000LL / frequency_);
    if (due.QuadPart == 0)
        return;
    SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
    MsgWaitForMultipleObjectsEx(1, &timer_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

void FramePacer::framePresented() {
    if (mode_ != Mode::timer || period_ <= 0)
        return;
    const long long current = now();
    nextDeadline_ += period_;
    // Fell behind by more than a frame (stall): restart the grid instead of bursting.
    if (current - nextDeadline_ > period_)
        nextDeadline_ = current + period_;
}

std::string FramePacer::describe() const {
    const std::string refresh = refreshHz_ > 0 ? std::format("{:.0f}Hz", refreshHz_) : std::string("不明");
    switch (mode_) {
    case Mode::unlimited:
        return std::format("無制限 (Present(0), 待機なし, リフレッシュ {})", refresh);
    case Mode::vsync:
        return std::format("{}fps: VSync分周 Present({}) (リフレッシュ {})", targetFps_, syncInterval_, refresh);
    case Mode::timer:
        return std::format("{}fps: 高精度タイマー{} + Present(0) (リフレッシュ {})", targetFps_,
                           highResolutionTimer_ ? "" : "(1ms)", refresh);
    }
    return "?";
}

double FramePacer::monitorRefreshRate(HWND hwnd) {
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return 0.0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
        return 0.0;
    // 0 and 1 mean "hardware default".
    return mode.dmDisplayFrequency > 1 ? static_cast<double>(mode.dmDisplayFrequency) : 0.0;
}

} // namespace uka3d
