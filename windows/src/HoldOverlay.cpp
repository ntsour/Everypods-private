#include "HoldOverlay.hpp"

#include "Logger.hpp"

#include <dwmapi.h>
#include <winreg.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace Propods {

namespace {

constexpr wchar_t kClassName[] = L"PropodsHoldOverlay";
constexpr UINT kMsgUpdate = WM_APP + 41;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 33;

// Defined here in case the SDK is older than Windows 11.
constexpr DWORD kDwmCornerPreference = 33;  // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr int kDwmRound = 2;                // DWMWCP_ROUND

constexpr int kWidth = 340;
constexpr int kHeightPlain = 96;
constexpr int kHeightButtons = 124;
constexpr int kMargin = 12;
constexpr int kPad = 16;

enum Button { kRetry = 0, kPlayHere = 1, kClose = 2 };

COLORREF blend(COLORREF a, COLORREF b, int pctB) {
    auto mix = [&](int x, int y) { return x + (y - x) * pctB / 100; };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
               mix(GetBValue(a), GetBValue(b)));
}

void roundRect(HDC dc, RECT r, int radius, COLORREF fill, const COLORREF* border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = border ? CreatePen(PS_SOLID, 1, *border)
                      : static_cast<HPEN>(GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    r.right += 1;  // RoundRect is exclusive on right/bottom; callers pass inclusive rects
    r.bottom += 1;
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    if (border) DeleteObject(pen);
    DeleteObject(brush);
}

void drawText(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF color, UINT flags) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r,
              flags | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, old);
}

std::wstring statusLine(const HoldStatus& s) {
    const bool isCall = s.kind == HoldKind::Call;
    const std::wstring& name = s.deviceName;
    switch (s.phase) {
        case HoldPhase::RequestingRelease: return L"Asking your phone to let go\u2026";
        case HoldPhase::Connecting:
            return isCall ? L"Moving your call to " + name + L"\u2026"
                          : L"Connecting to " + name + L"\u2026";
        case HoldPhase::Routing:
            return isCall ? L"Switching call audio\u2026" : L"Switching audio\u2026";
        case HoldPhase::Connected: return L"Connected";
        case HoldPhase::Failed: return L"Couldn\u2019t connect to " + name;
    }
    return {};
}

// GetDpiForWindow is Windows 10 1607+; resolve it at run time so neither the
// SDK's WINVER guard nor an older Windows can break the build or the start.
UINT dpiFor(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    const UINT dpi = fn ? fn(hwnd) : 0;
    return dpi ? dpi : 96;
}

bool animating(const HoldStatus& s) {
    return s.phase != HoldPhase::Failed && s.phase != HoldPhase::Connected;
}

}  // namespace

HoldOverlay::HoldOverlay(HINSTANCE instance, PlaybackHoldActions& actions)
    : m_instance(instance), m_actions(actions) {}

HoldOverlay::~HoldOverlay() {
    destroy();
    freeFonts();
}

bool HoldOverlay::create() {
    if (m_hwnd.load()) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &HoldOverlay::wndProc;
    wc.hInstance = m_instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log::error("HoldOverlay: RegisterClassExW failed: {}", GetLastError());
        return false;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName, L"EveryPods", WS_POPUP,
        0, 0, kWidth, kHeightPlain,
        nullptr, nullptr, m_instance, this);
    if (!hwnd) {
        log::error("HoldOverlay: CreateWindowExW failed: {}", GetLastError());
        return false;
    }
    m_hwnd.store(hwnd);

    const int pref = kDwmRound;
    DwmSetWindowAttribute(hwnd, kDwmCornerPreference, &pref, sizeof(pref));  // ignored on Windows 10

    loadTheme();
    m_dpi = dpiFor(hwnd);
    recreateFonts();

    // A status may have been stored before the window existed.
    applyStatus();
    return true;
}

void HoldOverlay::update(const std::optional<HoldStatus>& status) {
    {
        std::scoped_lock lk{m_mutex};
        m_pending = status;
    }
    if (HWND hwnd = m_hwnd.load()) PostMessageW(hwnd, kMsgUpdate, 0, 0);
}

void HoldOverlay::destroy() {
    HWND hwnd = m_hwnd.exchange(nullptr);
    if (hwnd) {
        if (m_timerOn) KillTimer(hwnd, kTimerId);
        m_timerOn = false;
        DestroyWindow(hwnd);
    }
    m_visible = false;
    m_shown.reset();
}

int HoldOverlay::scale(int logical) const {
    return MulDiv(logical, static_cast<int>(m_dpi), 96);
}

void HoldOverlay::loadTheme() {
    DWORD value = 1;  // missing = light
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        value = 1;
    Theme t;
    if (value == 0) {
        t.bg = RGB(0x20, 0x20, 0x20);
        t.text = RGB(0xFF, 0xFF, 0xFF);
        t.secondary = RGB(0xC8, 0xC8, 0xC8);
        t.accent = RGB(0x4C, 0xC2, 0xFF);
        t.border = RGB(0x3A, 0x3A, 0x3A);
        t.danger = RGB(0xE5, 0x48, 0x4D);
        t.hover = RGB(0x32, 0x32, 0x32);
        t.onAccent = RGB(0x00, 0x00, 0x00);  // white on #4CC2FF is unreadable
    } else {
        t.bg = RGB(0xF3, 0xF3, 0xF3);
        t.text = RGB(0x1A, 0x1A, 0x1A);
        t.secondary = RGB(0x5C, 0x5C, 0x5C);
        t.accent = RGB(0x00, 0x5F, 0xB8);
        t.border = RGB(0xD0, 0xD0, 0xD0);
        t.danger = RGB(0xC4, 0x2B, 0x1C);
        t.hover = RGB(0xE5, 0xE5, 0xE5);
        t.onAccent = RGB(0xFF, 0xFF, 0xFF);
    }
    m_theme = t;
}

void HoldOverlay::freeFonts() {
    if (m_titleFont) { DeleteObject(m_titleFont); m_titleFont = nullptr; }
    if (m_textFont) { DeleteObject(m_textFont); m_textFont = nullptr; }
}

void HoldOverlay::recreateFonts() {
    freeFonts();
    auto make = [&](int px, int weight) {
        return CreateFontW(-scale(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    };
    m_titleFont = make(14, FW_SEMIBOLD);
    m_textFont = make(13, FW_NORMAL);
}

void HoldOverlay::positionWindow(bool showButtons) {
    HWND hwnd = m_hwnd.load();
    if (!hwnd) return;
    RECT wa{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int w = scale(kWidth);
    const int h = scale(showButtons ? kHeightButtons : kHeightPlain);
    const int x = wa.right - scale(kMargin) - w;
    const int y = wa.bottom - scale(kMargin) - h;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

HoldOverlay::Layout HoldOverlay::layout() const {
    Layout l;
    const int w = scale(kWidth);
    const int h = scale(kHeightButtons);
    const int btnH = scale(28);
    const int gap = scale(8);
    const int bottom = h - scale(14);
    const int top = bottom - btnH;
    int right = w - scale(kPad);
    const bool call = m_shown && m_shown->kind == HoldKind::Call;
    const int playW = scale(call ? 80 : 96);
    l.playHere = RECT{right - playW, top, right, bottom};
    right -= playW + gap;
    l.retry = RECT{right - scale(76), top, right, bottom};
    const int c = scale(24);
    l.close = RECT{w - scale(10) - c, scale(8), w - scale(10), scale(8) + c};
    return l;
}

int HoldOverlay::hitTest(POINT pt) const {
    if (!m_shown || m_shown->phase != HoldPhase::Failed) return -1;
    const Layout l = layout();
    if (PtInRect(&l.retry, pt)) return kRetry;
    if (PtInRect(&l.playHere, pt)) return kPlayHere;
    if (PtInRect(&l.close, pt)) return kClose;
    return -1;
}

void HoldOverlay::applyStatus() {
    HWND hwnd = m_hwnd.load();
    if (!hwnd) return;
    {
        std::scoped_lock lk{m_mutex};
        m_shown = m_pending;
    }

    if (!m_shown) {
        if (m_timerOn) KillTimer(hwnd, kTimerId);
        m_timerOn = false;
        m_hover = m_pressed = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        ShowWindow(hwnd, SW_HIDE);
        m_visible = false;
        return;
    }

    // Refresh DPI-dependent resources in case the monitor/scale changed.
    UINT dpi = dpiFor(hwnd);
    if (dpi != m_dpi) {
        m_dpi = dpi;
        recreateFonts();
    }

    const bool failed = m_shown->phase == HoldPhase::Failed;
    if (!failed) m_hover = m_pressed = -1;
    positionWindow(failed);
    SetWindowTextW(hwnd, statusLine(*m_shown).c_str());

    if (!m_visible) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        m_visible = true;
    }

    if (animating(*m_shown)) {
        if (!m_timerOn) m_timerOn = SetTimer(hwnd, kTimerId, kTimerMs, nullptr) != 0;
    } else if (m_timerOn) {
        KillTimer(hwnd, kTimerId);
        m_timerOn = false;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void HoldOverlay::invoke(int button) {
    switch (button) {
        case kRetry: m_actions.retry(); break;
        case kPlayHere: m_actions.playHere(); break;
        case kClose: m_actions.dismiss(); break;
        default: break;
    }
}

void HoldOverlay::paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc) return;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = mem ? CreateCompatibleBitmap(hdc, w, h) : nullptr;
    if (!mem || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    const Theme& t = m_theme;

    // Background and 1px border.
    {
        HBRUSH bg = CreateSolidBrush(t.bg);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        HBRUSH bd = CreateSolidBrush(t.border);
        FrameRect(mem, &rc, bd);
        DeleteObject(bd);
    }

    if (m_shown) {
        const HoldStatus& s = *m_shown;
        const bool failed = s.phase == HoldPhase::Failed;
        const bool call = s.kind == HoldKind::Call;
        const int pad = scale(kPad);

        // Title.
        RECT title{pad, scale(14), w - pad - (failed ? scale(28) : 0), scale(34)};
        drawText(mem, s.deviceName, title, m_titleFont, t.text, DT_LEFT | DT_END_ELLIPSIS);

        // Status line.
        RECT status{pad, scale(37), w - pad, scale(57)};
        drawText(mem, statusLine(s), status, m_textFont, t.secondary, DT_LEFT | DT_END_ELLIPSIS);

        // Progress bar.
        const int barH = scale(4);
        const int barTop = scale(68);
        const int barW = w - 2 * pad;
        RECT track{pad, barTop, pad + barW, barTop + barH};
        roundRect(mem, RECT{track.left, track.top, track.right - 1, track.bottom - 1}, barH,
                  t.border, nullptr);
        const double p = std::clamp(s.progress(HoldStatus::Clock::now()), 0.0, 1.0);
        int fillW = static_cast<int>(barW * p);
        if (fillW > 0) {
            fillW = (std::max)(fillW, barH);
            RECT fill{pad, barTop, pad + fillW, barTop + barH};
            roundRect(mem, RECT{fill.left, fill.top, fill.right - 1, fill.bottom - 1}, barH,
                      failed ? t.danger : t.accent, nullptr);
        }

        if (failed) {
            const Layout l = layout();
            const int r = scale(6);

            // Retry: accent filled.
            {
                COLORREF fill = (m_hover == kRetry) ? blend(t.accent, RGB(255, 255, 255), 15)
                                                    : t.accent;
                roundRect(mem, l.retry, r, fill, nullptr);
                drawText(mem, L"Retry", l.retry, m_textFont, t.onAccent, DT_CENTER);
            }
            // Play on PC / Use PC: neutral outlined.
            {
                COLORREF fill = (m_hover == kPlayHere) ? t.hover : t.bg;
                roundRect(mem, l.playHere, r, fill, &t.border);
                drawText(mem, call ? L"Use PC" : L"Play on PC", l.playHere, m_textFont, t.text,
                         DT_CENTER);
            }
            // Close "x".
            {
                if (m_hover == kClose) roundRect(mem, l.close, r, t.hover, nullptr);
                drawText(mem, L"\u00D7", l.close, m_titleFont, t.secondary, DT_CENTER);
            }
        }
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK HoldOverlay::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HoldOverlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<HoldOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<HoldOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        try {
            return self->handle(hwnd, msg, wParam, lParam);
        } catch (...) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT HoldOverlay::handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case kMsgUpdate:
            applyStatus();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            paint(hwnd);
            return 0;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_TIMER:
            if (wParam == kTimerId) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_DPICHANGED:
            m_dpi = HIWORD(wParam);
            if (m_dpi == 0) m_dpi = 96;
            recreateFonts();
            positionWindow(m_shown && m_shown->phase == HoldPhase::Failed);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_SETTINGCHANGE:
            if (lParam && wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) {
                loadTheme();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                SetCursor(LoadCursorW(nullptr, hitTest(pt) >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            const int idx = hitTest(pt);
            if (idx != m_hover) {
                m_hover = idx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (!m_tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                if (TrackMouseEvent(&tme)) m_tracking = true;
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            m_tracking = false;
            if (m_hover != -1) {
                m_hover = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            const int idx = hitTest(pt);
            if (idx >= 0) {
                m_pressed = idx;
                SetCapture(hwnd);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            const int pressed = m_pressed;
            m_pressed = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (pressed >= 0) {
                POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
                if (hitTest(pt) == pressed) invoke(pressed);
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            m_pressed = -1;
            return 0;

        case WM_ENDSESSION:
            // Logoff / shutdown: the process may be killed right after this.
            if (wParam && m_onEndSession) m_onEndSession();
            return 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}
