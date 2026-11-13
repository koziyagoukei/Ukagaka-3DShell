#include "app/DesktopBehavior.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace uka3d {

const char *toString(BehaviorState state) {
    switch (state) {
    case BehaviorState::idle: return "Idle";
    case BehaviorState::walking: return "Walking";
    case BehaviorState::sitting: return "Sitting";
    case BehaviorState::transition: return "Transition";
    }
    return "Idle";
}

const char *toString(SeatPosition position) {
    switch (position) {
    case SeatPosition::left: return "top-left";
    case SeatPosition::center: return "top-center";
    case SeatPosition::right: return "top-right";
    }
    return "top-center";
}

bool DesktopWalk::advance(double elapsed) {
    const double dx = destination.x - position.x;
    const double dy = destination.y - position.y;
    const double distance = std::hypot(dx, dy);
    const double step = speed * std::max(elapsed, 0.0);
    if (step >= distance) {
        position = destination;
        return true;
    }
    position.x += dx * (step / distance);
    position.y += dy * (step / distance);
    return false;
}

RECT workArea(POINT point) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &info))
        return info.rcWork;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

std::vector<RECT> desktopWorkAreas() {
    std::vector<RECT> result;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info))
            reinterpret_cast<std::vector<RECT> *>(data)->push_back(info.rcWork);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

POINT clampToWorkArea(POINT position, SIZE size, const RECT &work) {
    return {std::clamp(position.x, work.left, std::max(work.left, work.right - size.cx)),
            std::clamp(position.y, work.top, std::max(work.top, work.bottom - size.cy))};
}

POINT seatPoint(const RECT &frame, SeatPosition position, LONG inset) {
    const LONG width = frame.right - frame.left;
    const LONG padding = std::clamp(inset, 0L, width / 2);
    if (position == SeatPosition::left)
        return {frame.left + padding, frame.top};
    if (position == SeatPosition::right)
        return {frame.right - 1 - padding, frame.top};
    return {frame.left + width / 2, frame.top};
}

POINT seatOrigin(POINT edge, POINT projectedHip, SIZE size, const RECT &work) {
    return clampToWorkArea({edge.x - projectedHip.x, edge.y - projectedHip.y}, size, work);
}

thread_local DesktopWindows *DesktopWindows::instance_ = nullptr;

DesktopWindows::DesktopWindows(DWORD excludedPid) : excludedPid_(excludedPid) {
    instance_ = this;
    rememberForeground(GetForegroundWindow());
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, event, 0, 0,
                                     WINEVENT_OUTOFCONTEXT);
}

DesktopWindows::~DesktopWindows() {
    watch(std::nullopt);
    if (foregroundHook_)
        UnhookWinEvent(foregroundHook_);
    instance_ = nullptr;
}

std::optional<DesktopWindow> DesktopWindows::inspect(HWND hwnd) const {
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd ||
        hwnd == GetShellWindow() || hwnd == GetDesktopWindow())
        return std::nullopt;

    DesktopWindow window;
    window.hwnd = hwnd;
    window.thread = GetWindowThreadProcessId(hwnd, &window.pid);
    if (!window.thread || window.pid == excludedPid_ || window.pid == GetCurrentProcessId())
        return std::nullopt;
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const auto extended = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (style & WS_CHILD || extended & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) ||
        (GetWindow(hwnd, GW_OWNER) && !(extended & WS_EX_APPWINDOW)))
        return std::nullopt;

    wchar_t className[128]{};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const std::wstring_view name(className);
    if (name == L"Ukagaka3dShellWindow" || name == L"Tmainform" || name == L"Progman" || name == L"WorkerW" ||
        name == L"Shell_TrayWnd" || name == L"Shell_SecondaryTrayWnd" || name == L"#32768" ||
        GetWindowTextLengthW(hwnd) == 0)
        return std::nullopt;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return std::nullopt;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd, &placement) || placement.showCmd == SW_SHOWMINIMIZED)
        return std::nullopt;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &window.frame, sizeof(window.frame))) &&
        !GetWindowRect(hwnd, &window.frame))
        return std::nullopt;
    const HMONITOR monitor = MonitorFromRect(&window.frame, MONITOR_DEFAULTTONULL);
    if (window.frame.right - window.frame.left < 120 || window.frame.bottom - window.frame.top < 60 || !monitor)
        return std::nullopt;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!(style & WS_CAPTION) && GetMonitorInfoW(monitor, &info) &&
        window.frame.left <= info.rcMonitor.left && window.frame.top <= info.rcMonitor.top &&
        window.frame.right >= info.rcMonitor.right && window.frame.bottom >= info.rcMonitor.bottom)
        return std::nullopt;
    return window;
}

std::optional<DesktopWindow> DesktopWindows::foreground() const {
    if (auto window = inspect(GetForegroundWindow()))
        return window;
    if (lastForeground_) {
        const auto window = inspect(lastForeground_->hwnd);
        if (window && window->thread == lastForeground_->thread && window->pid == lastForeground_->pid)
            return window;
    }
    return std::nullopt;
}

std::vector<DesktopWindow> DesktopWindows::list() const {
    std::vector<DesktopWindow> result;
    struct Enumeration {
        const DesktopWindows &windows;
        std::vector<DesktopWindow> &result;
    } enumeration{*this, result};
    EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
        auto &data = *reinterpret_cast<Enumeration *>(parameter);
        if (auto window = data.windows.inspect(hwnd))
            data.result.push_back(*window);
        return data.result.size() < 128;
    }, reinterpret_cast<LPARAM>(&enumeration));
    return result;
}

void DesktopWindows::watch(const std::optional<DesktopWindow> &window) {
    if (objectHook_)
        UnhookWinEvent(objectHook_);
    if (minimizeHook_)
        UnhookWinEvent(minimizeHook_);
    objectHook_ = minimizeHook_ = nullptr;
    watched_ = window ? window->hwnd : nullptr;
    changed_ = destroyed_ = false;
    if (window) {
        objectHook_ = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr, event,
                                     window->pid, window->thread, WINEVENT_OUTOFCONTEXT);
        minimizeHook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr, event,
                                       window->pid, window->thread, WINEVENT_OUTOFCONTEXT);
    }
}

bool DesktopWindows::changed() {
    return std::exchange(changed_, false);
}

void DesktopWindows::rememberForeground(HWND hwnd) {
    if (auto window = inspect(hwnd))
        lastForeground_ = window;
}

void CALLBACK DesktopWindows::event(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
    auto *self = instance_;
    if (!self)
        return;
    if (hook == self->foregroundHook_) {
        self->rememberForeground(hwnd);
        if (self->watched_)
            self->changed_ = true;
        return;
    }
    if (hwnd != self->watched_ || child != CHILDID_SELF || object != OBJID_WINDOW)
        return;
    if (hook == self->minimizeHook_ || (hook == self->objectHook_ && event == EVENT_OBJECT_DESTROY))
        self->destroyed_ = true; // also handles a handle being recycled before the next tick
    self->changed_ = true;
}

} // namespace uka3d
