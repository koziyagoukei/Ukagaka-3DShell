#include "platform/TransparentWindow.h"

#include "platform/Log.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace uka3d {
namespace {

constexpr wchar_t kClassName[] = L"Ukagaka3dShellWindow";
constexpr UINT_PTR kModalRenderTimer = 1;
constexpr UINT_PTR kGrabTimer = 2;

void registerClassOnce(HINSTANCE instance, WNDPROC proc) {
    static bool registered = false;
    if (registered)
        return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kClassName;
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc))
        throw std::runtime_error("RegisterClassExW failed");
    registered = true;
}

} // namespace

ShellWindow::ShellWindow(const std::wstring &title, int width, int height, bool transparent, bool topmost,
                         Callbacks callbacks, bool hideFromTaskbar)
    : transparent_(transparent), topmost_(topmost), callbacks_(std::move(callbacks)) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    registerClassOnce(instance, &ShellWindow::windowProc);

    DWORD style = 0;
    DWORD exStyle = 0;
    if (transparent_) {
        style = WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU;
        exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW;
    } else {
        style = WS_OVERLAPPEDWINDOW;
        exStyle = WS_EX_APPWINDOW;
    }
    if (topmost_)
        exStyle |= WS_EX_TOPMOST;
    if (hideFromTaskbar)
        exStyle = (exStyle & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW; // like an SSP shell window

    // Sizes are logical (96 dpi) pixels; this process is per-monitor DPI aware.
    const UINT dpi = GetDpiForSystem();
    width = MulDiv(width, static_cast<int>(dpi), 96);
    height = MulDiv(height, static_cast<int>(dpi), 96);
    RECT rect{0, 0, width, height};
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    const int outerW = rect.right - rect.left;
    const int outerH = rect.bottom - rect.top;

    // Initial position: bottom-right of the primary work area (mascot style).
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = transparent_ ? std::max<int>(work.left, work.right - outerW - 32) : CW_USEDEFAULT;
    const int y = transparent_ ? std::max<int>(work.top, work.bottom - outerH) : CW_USEDEFAULT;

    hwnd_ = CreateWindowExW(exStyle, kClassName, title.c_str(), style, x, y, outerW, outerH, nullptr, nullptr,
                            instance, this);
    if (hwnd_ == nullptr)
        throw std::runtime_error("CreateWindowExW failed: " + log::hresultToString(HRESULT_FROM_WIN32(GetLastError())));
    log::info("ウィンドウ作成: {} {}x{}{}", transparent_ ? "背景透過" : "通常", width, height,
              topmost_ ? " 最前面" : "");
}

ShellWindow::~ShellWindow() {
    if (hwnd_ != nullptr) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

void ShellWindow::postWake() const {
    PostMessageW(hwnd_, kWakeMessage, 0, 0);
}

void ShellWindow::show() {
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
}

SIZE ShellWindow::clientSize() const {
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    return {rect.right - rect.left, rect.bottom - rect.top};
}

void ShellWindow::setClickThrough(bool enabled) {
    if (!transparent_ || enabled == clickThrough_)
        return;
    log::debug("クリック透過: {}", enabled ? "ON" : "OFF");
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (enabled) {
        ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
        // A layered window needs attributes before it is shown again; keep it fully opaque.
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    } else {
        ex &= ~(WS_EX_LAYERED | WS_EX_TRANSPARENT);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    }
    clickThrough_ = enabled;
    log::debug("クリック透過: {}", enabled ? "ON" : "OFF");
}

void ShellWindow::setTopmost(bool enabled) {
    topmost_ = enabled;
    SetWindowPos(hwnd_, (topmost_ || seatForeground_) ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ShellWindow::setSeatForeground(bool enabled) {
    if (seatForeground_ == enabled)
        return;
    seatForeground_ = enabled;
    if (!topmost_)
        setTopmost(false);
}

bool ShellWindow::forwardMouse(UINT message, WPARAM wParam, LPARAM lParam) {
    if (forwardTarget_ == nullptr || !IsWindow(forwardTarget_))
        return false;
    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (dragTarget_ != nullptr) {
        ClientToScreen(hwnd_, &pt);
        ScreenToClient(forwardTarget_, &pt);
    } else {
        // Desktop behavior may leave the SSP controls at the previous position.
        const SIZE size = clientSize();
        RECT target{};
        if (size.cx > 0 && size.cy > 0 && GetClientRect(forwardTarget_, &target)) {
            pt.x = MulDiv(pt.x, target.right, size.cx);
            pt.y = MulDiv(pt.y, target.bottom, size.cy);
        }
    }
    // We are the foreground process right now; hand the foreground to the
    // target so that its popup menu works normally.
    DWORD targetPid = 0;
    GetWindowThreadProcessId(forwardTarget_, &targetPid);
    AllowSetForegroundWindow(targetPid);
    SetForegroundWindow(forwardTarget_);
    PostMessageW(forwardTarget_, message, wParam, MAKELPARAM(pt.x, pt.y));
    return true;
}

void ShellWindow::beginDrag(POINT client) {
    if (dragStarted_)
        return;
    dragStarted_ = true;
    if (callbacks_.onPointer)
        callbacks_.onPointer(PointerAction::dragStart, client, false);
    if (callbacks_.onDragStart)
        callbacks_.onDragStart();
}

void ShellWindow::moveDrag(POINT screen) {
    HWND target = dragTarget_ != nullptr && IsWindow(dragTarget_) ? dragTarget_ : hwnd_;
    SetWindowPos(target, nullptr, screen.x - dragOffset_.x, screen.y - dragOffset_.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ShellWindow::pressLeft(POINT client, bool doubleClick) {
    if (callbacks_.onPointer)
        callbacks_.onPointer(doubleClick ? PointerAction::doublePress : PointerAction::press, client, false);
    if (!transparent_ && !callbacks_.onPointer)
        return;

    POINT cursor = client;
    if (callbacks_.onPointer)
        ClientToScreen(hwnd_, &cursor);
    else
        GetCursorPos(&cursor);
    RECT rect{};
    GetWindowRect(dragTarget_ != nullptr && IsWindow(dragTarget_) ? dragTarget_ : hwnd_, &rect);
    dragOffset_ = {cursor.x - rect.left, cursor.y - rect.top};
    dragPress_ = cursor;
    dragCursor_ = cursor;
    pressMoved_ = false;
    dragging_ = true;
    SetCapture(hwnd_);
    const UINT delay = grabDelay_ != 0 ? grabDelay_ : (callbacks_.onPointer ? 180U : 0U);
    if (transparent_ && delay != 0)
        SetTimer(hwnd_, kGrabTimer, delay, nullptr);
}

void ShellWindow::setBounds(const RECT &rect) {
    RECT current{};
    GetWindowRect(hwnd_, &current);
    if (EqualRect(&current, &rect))
        return;
    SetWindowPos(hwnd_, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void ShellWindow::moveTo(POINT position) {
    SetWindowPos(hwnd_, nullptr, position.x, position.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ShellWindow::scaleBy(float factor) {
    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    const int w = rect.right - rect.left;
    const int h = rect.bottom - rect.top;
    const int newW = std::clamp(static_cast<int>(w * factor), 96, 4096);
    const int newH = std::clamp(static_cast<int>(h * factor), 96, 4096);
    // Keep the bottom centre anchored (the character's feet stay in place).
    const int cx = rect.left + w / 2;
    SetWindowPos(hwnd_, nullptr, cx - newW / 2, rect.bottom - newH, newW, newH, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool ShellWindow::pumpMessages(int &exitCode) {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            exitCode = static_cast<int>(msg.wParam);
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

LRESULT CALLBACK ShellWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
        auto *self = static_cast<ShellWindow *>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto *self = reinterpret_cast<ShellWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr)
        return DefWindowProcW(hwnd, message, wParam, lParam);
    return self->handleMessage(message, wParam, lParam);
}

LRESULT ShellWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_DPICHANGED:
        setBounds(*reinterpret_cast<const RECT *>(lParam));
        log::info("ウィンドウ DPI: {}", LOWORD(wParam));
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && callbacks_.onResize)
            callbacks_.onResize(LOWORD(lParam), HIWORD(lParam));
        if (inSizeMove_ && callbacks_.onRenderRequest)
            callbacks_.onRenderRequest();
        return 0;
    case WM_ENTERSIZEMOVE:
        // The modal move/size loop blocks our main loop; keep animating via a timer.
        inSizeMove_ = true;
        SetTimer(hwnd_, kModalRenderTimer, 16, nullptr);
        return 0;
    case WM_EXITSIZEMOVE:
        inSizeMove_ = false;
        KillTimer(hwnd_, kModalRenderTimer);
        return 0;
    case WM_TIMER:
        if (wParam == kModalRenderTimer && callbacks_.onRenderRequest)
            callbacks_.onRenderRequest();
        if (wParam == kGrabTimer && dragging_ && !grabbed_) {
            KillTimer(hwnd_, kGrabTimer);
            grabbed_ = true;
            if (!callbacks_.onPointer) {
                beginDrag({});
            } else if (pressMoved_) {
                POINT client = dragCursor_;
                ScreenToClient(hwnd_, &client);
                beginDrag(client);
                moveDrag(dragCursor_);
            }
            if (grabDelay_ != 0 && callbacks_.onGrabStart)
                callbacks_.onGrabStart();
        }
        return 0;
    case WM_LBUTTONDOWN:
        pressLeft({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, false);
        return 0;
    case WM_MOUSEMOVE: {
        const POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (dragging_) {
            POINT cursor = client;
            if (callbacks_.onPointer)
                ClientToScreen(hwnd_, &cursor);
            else
                GetCursorPos(&cursor);
            dragCursor_ = cursor;
            const UINT dpi = GetDpiForWindow(hwnd_);
            pressMoved_ = pressMoved_ || std::abs(cursor.x - dragPress_.x) >= GetSystemMetricsForDpi(SM_CXDRAG, dpi) ||
                          std::abs(cursor.y - dragPress_.y) >= GetSystemMetricsForDpi(SM_CYDRAG, dpi);
            if (!transparent_ || (!dragStarted_ && (!pressMoved_ || (callbacks_.onPointer && !grabbed_))))
                return 0;
            beginDrag(client);
            moveDrag(cursor);
        } else if (callbacks_.onPointer) {
            if (!trackingMouse_) {
                TRACKMOUSEEVENT request{sizeof(request), TME_LEAVE, hwnd_, 0};
                trackingMouse_ = TrackMouseEvent(&request) != FALSE;
            }
            constexpr WPARAM buttons = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;
            callbacks_.onPointer(PointerAction::move, client, (wParam & buttons) != 0);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        if (callbacks_.onPointer)
            callbacks_.onPointer(PointerAction::leave, {}, false);
        return 0;
    case WM_LBUTTONUP:
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        if (dragging_) {
            if (callbacks_.onPointer) {
                if (dragStarted_) {
                    POINT cursor{};
                    GetCursorPos(&cursor);
                    ScreenToClient(hwnd_, &cursor);
                    callbacks_.onPointer(PointerAction::dragEnd, cursor, false);
                } else if (message == WM_LBUTTONUP) {
                    callbacks_.onPointer(PointerAction::release, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)},
                                         grabbed_ || pressMoved_);
                } else {
                    callbacks_.onPointer(PointerAction::cancel, {}, false);
                }
            }
            dragging_ = false;
            dragStarted_ = false;
            KillTimer(hwnd_, kGrabTimer);
            if (message != WM_CAPTURECHANGED && GetCapture() == hwnd_)
                ReleaseCapture();
            if (grabbed_) {
                grabbed_ = false;
                if (grabDelay_ != 0 && callbacks_.onGrabEnd)
                    callbacks_.onGrabEnd();
            }
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (callbacks_.onPointer)
            callbacks_.onPointer(PointerAction::leave, {}, false);
        if (forwardTarget_ != nullptr && !(wParam & MK_SHIFT) && forwardMouse(WM_RBUTTONDOWN, wParam, lParam))
            return 0;
        break;
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        if (callbacks_.onPointer)
            callbacks_.onPointer(PointerAction::leave, {}, false);
        break;
    case WM_LBUTTONDBLCLK:
        if (callbacks_.onPointer) {
            pressLeft({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, true);
            return 0;
        }
        if (forwardMouse(WM_LBUTTONDOWN, MK_LBUTTON, lParam)) {
            forwardMouse(WM_LBUTTONDBLCLK, wParam, lParam);
            forwardMouse(WM_LBUTTONUP, 0, lParam);
            return 0;
        }
        break;
    case WM_RBUTTONUP: {
        if (forwardTarget_ != nullptr && !(wParam & MK_SHIFT) && GetKeyState(VK_SHIFT) >= 0 &&
            forwardMouse(WM_RBUTTONUP, wParam, lParam))
            return 0;
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd_, &pt);
        if (callbacks_.onContextMenu)
            callbacks_.onContextMenu(pt);
        return 0;
    }
    case WM_CONTEXTMENU:
        if (lParam == -1 && callbacks_.onContextMenu) { // keyboard (Shift+F10 / menu key)
            RECT rect{};
            GetWindowRect(hwnd_, &rect);
            callbacks_.onContextMenu({rect.left + 16, rect.top + 16});
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (callbacks_.onWheel)
            callbacks_.onWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA,
                               (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0);
        return 0;
    case WM_KEYDOWN:
        if (callbacks_.onKeyDown)
            callbacks_.onKeyDown(wParam);
        return 0;
    case WM_CLOSE:
        if (callbacks_.onClose)
            callbacks_.onClose();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_COPYDATA:
        // SSTP responses are optional for the runtime; the SSP event itself
        // has already been handled and no response payload is consumed here.
        if (callbacks_.onPointer)
            return TRUE;
        break;
    case kWakeMessage:
        if (callbacks_.onWake)
            callbacks_.onWake();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

} // namespace uka3d
