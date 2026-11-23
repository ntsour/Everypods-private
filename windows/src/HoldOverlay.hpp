#pragma once

// HoldOverlay: the small floating panel that shows AirPods handover progress
// while PlaybackHold is holding this PC's audio (the Windows counterpart of the
// Mac NSPanel).
//
// Never steals focus: the user is usually in a full-screen browser video, and
// taking activation would pause or un-fullscreen it. The window is therefore
//   - WS_POPUP with WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW
//     (no taskbar button, never the foreground window),
//   - shown only with SW_SHOWNOACTIVATE / SWP_NOACTIVATE,
//   - answers WM_MOUSEACTIVATE with MA_NOACTIVATE so clicking the Retry /
//     Play on PC buttons does not activate it either.
// Everything is custom-drawn with GDI (double buffered); there are no child
// controls, because child controls would take focus.
//
// Threading: create()/destroy() and all painting run on the UI thread (the one
// pumping messages). update() may be called from any thread: it stores the
// status under a mutex and posts a private message to the window.

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>

#include "PlaybackHold.hpp"

namespace Propods {

class HoldOverlay {
public:
    HoldOverlay(HINSTANCE instance, PlaybackHoldActions& actions);
    ~HoldOverlay();

    HoldOverlay(const HoldOverlay&) = delete;
    HoldOverlay& operator=(const HoldOverlay&) = delete;

    // Call on the UI thread (the thread that runs the tray's message loop), once.
    bool create();
    // Any thread. nullopt hides the panel. Marshals to the UI thread with PostMessage.
    void update(const std::optional<HoldStatus>& status);
    // UI thread. Destroys the window.
    void destroy();
    // Called on the UI thread on WM_ENDSESSION (logoff / shutdown). The tray's
    // window is message-only and never gets it; this top-level one does.
    void setOnEndSession(std::function<void()> cb) { m_onEndSession = std::move(cb); }

private:
    struct Theme {
        COLORREF bg = 0, text = 0, secondary = 0, accent = 0, border = 0,
                 danger = 0, hover = 0, onAccent = 0;
    };
    struct Layout {
        RECT retry{}, playHere{}, close{};
    };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void applyStatus();        // UI thread: take snapshot and show/hide/refresh
    void loadTheme();
    void recreateFonts();
    void freeFonts();
    void positionWindow(bool showButtons);
    Layout layout() const;
    int hitTest(POINT pt) const;  // -1 none, 0 retry, 1 playHere, 2 close
    void paint(HWND hwnd);
    void invoke(int button);
    int scale(int logical) const;

    HINSTANCE m_instance;
    PlaybackHoldActions& m_actions;
    std::function<void()> m_onEndSession;
    std::atomic<HWND> m_hwnd{nullptr};

    // Latest status from any thread.
    std::mutex m_mutex;
    std::optional<HoldStatus> m_pending;

    // UI-thread-only state.
    std::optional<HoldStatus> m_shown;
    Theme m_theme;
    UINT m_dpi = 96;
    HFONT m_titleFont = nullptr;
    HFONT m_textFont = nullptr;
    int m_hover = -1;
    int m_pressed = -1;
    bool m_tracking = false;
    bool m_timerOn = false;
    bool m_visible = false;
};

}
