#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace uka3d {

// Win32 top-level window for the shell.
//
// transparent=true : borderless popup with WS_EX_NOREDIRECTIONBITMAP, meant to
//                    be filled by a DirectComposition visual. Left-drag moves
//                    the window; click-through can be toggled per frame.
// transparent=false: ordinary resizable overlapped window.
class ShellWindow {
  public:
    enum class PointerAction { move, leave, press, doublePress, release, cancel, dragStart, dragEnd };

    struct Callbacks {
        std::function<void(UINT width, UINT height)> onResize;
        std::function<void()> onRenderRequest; // during modal move/size loops
        std::function<void(WPARAM key)> onKeyDown;
        std::function<void(POINT screen)> onContextMenu;
        std::function<void(float notches, bool ctrl)> onWheel;
        std::function<void()> onClose;
        std::function<void()> onWake; // posted by other threads (IPC); runs on the UI thread
        std::function<void()> onDragStart;
        std::function<void()> onGrabStart;
        std::function<void()> onGrabEnd;
        // Optional 3D interaction handler. Replaces legacy left-double-click forwarding.
        std::function<void(PointerAction, POINT, bool)> onPointer;
    };

    static constexpr UINT kWakeMessage = WM_APP + 1;
    // Thread-safe: asks the UI thread to run Callbacks::onWake.
    void postWake() const;

    ShellWindow(const std::wstring &title, int width, int height, bool transparent, bool topmost,
                Callbacks callbacks, bool hideFromTaskbar = false);
    ~ShellWindow();
    ShellWindow(const ShellWindow &) = delete;
    ShellWindow &operator=(const ShellWindow &) = delete;

    void show();
    [[nodiscard]] HWND hwnd() const noexcept {
        return hwnd_;
    }
    [[nodiscard]] bool transparent() const noexcept {
        return transparent_;
    }
    [[nodiscard]] SIZE clientSize() const;

    // Makes mouse input pass through to windows below (transparent mode only).
    void setClickThrough(bool enabled);
    [[nodiscard]] bool clickThrough() const noexcept {
        return clickThrough_;
    }
    void setTopmost(bool enabled);
    void setSeatForeground(bool enabled);
    [[nodiscard]] bool topmost() const noexcept {
        return topmost_;
    }
    [[nodiscard]] bool dragging() const noexcept {
        return dragging_;
    }
    // While set, left-dragging the model moves `target` (e.g. the SSP shell
    // window this window follows) instead of this window.
    void setDragTarget(HWND target) {
        dragTarget_ = target;
    }
    // While set, right-clicks and left double-clicks on the model are
    // forwarded to `target` (SSP's shell window: opens SSP's menu and raises
    // OnMouseDoubleClick). Shift+right-click still opens our own menu.
    void setForwardTarget(HWND target) {
        forwardTarget_ = target;
    }
    // Moves/resizes the window to `rect` (screen coordinates, physical pixels).
    void setBounds(const RECT &rect);
    void moveTo(POINT position);
    void setGrabDelay(UINT milliseconds) { grabDelay_ = milliseconds; }
    // Scales the window around its centre (transparent mode resize).
    void scaleBy(float factor);

    // Pumps pending messages. Returns false once WM_QUIT was received.
    static bool pumpMessages(int &exitCode);

  private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    HWND hwnd_{};
    bool transparent_;
    bool topmost_;
    bool seatForeground_{false};
    bool clickThrough_{false};
    bool dragging_{false};
    bool dragStarted_{false};
    bool grabbed_{false};
    bool pressMoved_{false};
    bool trackingMouse_{false};
    bool inSizeMove_{false};
    UINT grabDelay_{};
    POINT dragPress_{};
    POINT dragCursor_{};
    POINT dragOffset_{};
    HWND dragTarget_{nullptr};
    HWND forwardTarget_{nullptr};
    bool forwardMouse(UINT message, WPARAM wParam, LPARAM lParam);
    void beginDrag(POINT client);
    void moveDrag(POINT screen);
    void pressLeft(POINT client, bool doubleClick);
    Callbacks callbacks_;
};

} // namespace uka3d
