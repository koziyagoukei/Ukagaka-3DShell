#pragma once

#include <windows.h>

#include <optional>
#include <vector>

namespace uka3d {

enum class BehaviorState { idle, walking, sitting, transition };
enum class SeatPosition { left, center, right };
const char *toString(BehaviorState state);
const char *toString(SeatPosition position);

struct DesktopPoint {
    double x{};
    double y{};
};

struct DesktopWalk {
    DesktopPoint position;
    DesktopPoint destination;
    double speed{}; // physical pixels per second
    bool advance(double elapsed);
};

RECT workArea(POINT point);
std::vector<RECT> desktopWorkAreas();
POINT clampToWorkArea(POINT position, SIZE size, const RECT &work);
POINT seatPoint(const RECT &frame, SeatPosition position, LONG inset);
POINT seatOrigin(POINT edge, POINT projectedHip, SIZE size, const RECT &work);

struct DesktopWindow {
    HWND hwnd{};
    DWORD pid{};
    DWORD thread{};
    RECT frame{};
};

// UI-thread only. Foreground changes record the candidate, and target changes
// only mark it dirty. The normal application loop updates placement.
class DesktopWindows {
  public:
    explicit DesktopWindows(DWORD excludedPid);
    ~DesktopWindows();
    DesktopWindows(const DesktopWindows &) = delete;
    DesktopWindows &operator=(const DesktopWindows &) = delete;

    [[nodiscard]] std::optional<DesktopWindow> inspect(HWND hwnd) const;
    [[nodiscard]] std::optional<DesktopWindow> foreground() const;
    [[nodiscard]] std::vector<DesktopWindow> list() const;
    void watch(const std::optional<DesktopWindow> &window);
    bool changed();
    [[nodiscard]] bool destroyed() const { return destroyed_; }

  private:
    static void CALLBACK event(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD);
    void rememberForeground(HWND hwnd);

    static thread_local DesktopWindows *instance_;
    DWORD excludedPid_;
    std::optional<DesktopWindow> lastForeground_;
    HWND watched_{};
    HWINEVENTHOOK foregroundHook_{};
    HWINEVENTHOOK objectHook_{};
    HWINEVENTHOOK minimizeHook_{};
    bool changed_{};
    bool destroyed_{};
};

} // namespace uka3d
