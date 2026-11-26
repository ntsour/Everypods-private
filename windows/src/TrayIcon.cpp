#include "TrayIcon.hpp"

#include "AirPodsIcon.hpp"
#include "Logger.hpp"

#include <strsafe.h>
#include <winreg.h>

// Icon resource ID (defined in Propods.rc)
#define IDI_APP 1

namespace Propods {

namespace {
TrayIcon* g_instance = nullptr;
constexpr wchar_t kClassName[]   = L"PropodsTrayWindow";
constexpr wchar_t kRunKeyPath[]  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"Propods";
constexpr UINT kIconId = 1;

bool isAutoStartEnabled() {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    bool exists = (RegQueryValueExW(hk, kRunValueName, nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
    RegCloseKey(hk);
    return exists;
}

void setAutoStart(bool enable) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        RegSetValueExW(hk, kRunValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(exePath),
            static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hk, kRunValueName);
    }
    RegCloseKey(hk);
}
}

TrayIcon::TrayIcon() {
    g_instance = this;
}

TrayIcon::~TrayIcon() {
    if (m_hwnd) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_ownIcon) DestroyIcon(m_ownIcon);
    g_instance = nullptr;
}

bool TrayIcon::create(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayIcon::wndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        0, kClassName, L"Propods",
        WS_OVERLAPPED,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!m_hwnd) {
        log::error("CreateWindowExW failed: {}", GetLastError());
        return false;
    }

    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = m_hwnd;
    m_nid.uID = kIconId;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAY;
    m_nid.hIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    StringCchCopyW(m_nid.szTip, ARRAYSIZE(m_nid.szTip), m_statusText.c_str());
    if (!Shell_NotifyIconW(NIM_ADD, &m_nid)) {
        log::error("Shell_NotifyIconW(NIM_ADD) failed");
        return false;
    }
    return true;
}

void TrayIcon::setStatus(const std::wstring& text) {
    m_statusText = text;
    if (!m_hwnd) return;
    m_nid.uFlags = NIF_TIP;
    StringCchCopyW(m_nid.szTip, ARRAYSIZE(m_nid.szTip), m_statusText.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

namespace {
// The taskbar has its own light/dark setting, separate from apps'.
bool taskbarIsLight() {
    DWORD value = 0;  // missing = dark taskbar (the Windows 11 default)
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value != 0;
}
}

void TrayIcon::setIconState(IconState state) {
    if (!m_hwnd) return;
    std::scoped_lock lk{m_iconMutex};
    if (m_iconSet && state == m_iconState) return;
    const bool light = taskbarIsLight();
    COLORREF color = 0;
    switch (state) {
        case IconState::Here:       color = light ? RGB(0x0F, 0x7B, 0x0F) : RGB(0x6C, 0xCB, 0x5F); break;
        case IconState::Connecting: color = light ? RGB(0x00, 0x5F, 0xB8) : RGB(0x4C, 0xC2, 0xFF); break;
        case IconState::Away:       color = light ? RGB(0x70, 0x70, 0x70) : RGB(0x9A, 0x9A, 0x9A); break;
    }
    HICON icon = createAirPodsIcon(GetSystemMetrics(SM_CXSMICON), color);
    if (!icon) {
        log::warn("Tray: could not draw the AirPods icon; keeping the current one");
        return;
    }
    NOTIFYICONDATAW nid = m_nid;  // copy: only the icon changes
    nid.uFlags = NIF_ICON;
    nid.hIcon = icon;
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        DestroyIcon(icon);
        return;
    }
    m_nid.hIcon = icon;
    if (m_ownIcon) DestroyIcon(m_ownIcon);
    m_ownIcon = icon;
    m_iconState = state;
    m_iconSet = true;
}

void TrayIcon::notify(const std::wstring& title, const std::wstring& text) {
    if (!m_hwnd) return;
    NOTIFYICONDATAW nid = m_nid;  // copy: only the balloon fields change
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    StringCchCopyW(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title.c_str());
    StringCchCopyW(nid.szInfo, ARRAYSIZE(nid.szInfo), text.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

LRESULT CALLBACK TrayIcon::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_instance) return g_instance->handleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayIcon::handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                showContextMenu(hwnd);
            }
            return 0;
        case WM_COMMAND:
            if (m_menuHandler) m_menuHandler((int)LOWORD(wp));
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void TrayIcon::showContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    // Fetch live status just before the menu opens.
    StatusInfo status;
    if (m_statusProvider) status = m_statusProvider();

    HMENU menu = CreatePopupMenu();

    // ── AirPods ──────────────────────────────────────────────────────────────
    // Ownership line (e.g. "AirPods: on this PC")
    InsertMenuW(menu, static_cast<UINT>(-1),
        MF_BYPOSITION | MF_STRING | MF_DISABLED | MF_GRAYED,
        0u, m_statusText.c_str());

    // Physical BT connection state with checkmark when connected.
    const wchar_t* airpodsDetail = status.airpodsConnected
        ? L"AirPods connected (Bluetooth)"
        : L"AirPods not connected";
    UINT airpodsFlags = MF_BYPOSITION | MF_STRING | MF_DISABLED | MF_GRAYED;
    if (status.airpodsConnected) airpodsFlags |= MF_CHECKED;
    InsertMenuW(menu, static_cast<UINT>(-1), airpodsFlags, 0u, airpodsDetail);

    InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_SEPARATOR, 0u, nullptr);

    // ── Peers ────────────────────────────────────
    if (!status.peers.empty()) {
        InsertMenuW(menu, static_cast<UINT>(-1),
            MF_BYPOSITION | MF_STRING | MF_DISABLED | MF_GRAYED,
            0u, L"Peers:");
        for (int i = 0; i < (int)status.peers.size(); ++i) {
            auto& peer = status.peers[i];
            UINT peerFlags = MF_BYPOSITION | MF_STRING | MF_DISABLED | MF_GRAYED;
            if (peer.connected) peerFlags |= MF_CHECKED;
            InsertMenuW(menu, static_cast<UINT>(-1), peerFlags, 0u, peer.name.c_str());
            std::wstring removeLabel = L"Remove " + peer.name;
            InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING,
                static_cast<UINT_PTR>(kCmdRemovePeer + i), removeLabel.c_str());
        }
        InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_SEPARATOR, 0u, nullptr);
    }

    // ── Actions ───────────────────────────────────────────────────────────────
    InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING,
        static_cast<UINT_PTR>(kCmdPairAndroid), L"Add peer...");
    InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING,
        static_cast<UINT_PTR>(kCmdPairAirPods), L"Select AirPods...");
    InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING,
        static_cast<UINT_PTR>(kCmdConnectAirPods), L"Connect AirPods now");
    const UINT onPcFlags = MF_BYPOSITION | MF_STRING | (status.airpodsConnected ? 0u : MF_GRAYED);
    InsertMenuW(menu, static_cast<UINT>(-1), onPcFlags,
        static_cast<UINT_PTR>(kCmdConnectMic), L"Connect AirPods microphone");
    InsertMenuW(menu, static_cast<UINT>(-1), onPcFlags,
        static_cast<UINT_PTR>(kCmdUseAirPodsAudio), L"Use AirPods for sound and mic");
    InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_SEPARATOR, 0u, nullptr);

    UINT autoRouteFlags = MF_BYPOSITION | MF_STRING;
    if (status.autoRouteEnabled) autoRouteFlags |= MF_CHECKED;
    InsertMenuW(menu, static_cast<UINT>(-1), autoRouteFlags,
        static_cast<UINT_PTR>(kCmdToggleAutoRoute), L"Auto-set AirPods as audio device");

    UINT pauseFlags = MF_BYPOSITION | MF_STRING;
    if (status.pauseOnRelease) pauseFlags |= MF_CHECKED;
    InsertMenuW(menu, static_cast<UINT>(-1), pauseFlags,
        static_cast<UINT_PTR>(kCmdTogglePause), L"Pause PC audio when AirPods leave");
    UINT takeMediaFlags = MF_BYPOSITION | MF_STRING;
    if (status.takeOnMediaStart) takeMediaFlags |= MF_CHECKED;
    InsertMenuW(menu, static_cast<UINT>(-1), takeMediaFlags,
        static_cast<UINT_PTR>(kCmdToggleTakeOnMedia), L"Take AirPods when this PC starts playing media");
    UINT takeCallFlags = MF_BYPOSITION | MF_STRING;
    if (status.takeOnCall) takeCallFlags |= MF_CHECKED;
    InsertMenuW(menu, static_cast<UINT>(-1), takeCallFlags,
        static_cast<UINT_PTR>(kCmdToggleTakeOnCall), L"Take AirPods when a call starts on this PC");
    UINT holdFlags = MF_BYPOSITION | MF_STRING;
    if (status.holdPlayback) holdFlags |= MF_CHECKED;
    InsertMenuW(menu, static_cast<UINT>(-1), holdFlags,
        static_cast<UINT_PTR>(kCmdToggleHold), L"Hold PC audio while AirPods connect");
    UINT lidFlags = MF_BYPOSITION | MF_STRING;
    if (status.lidOpenConnect) lidFlags |= MF_CHECKED;
    InsertMenuW(menu, static_cast<UINT>(-1), lidFlags,
        static_cast<UINT_PTR>(kCmdToggleLidOpen), L"Connect AirPods when the case opens (if this PC used them last)");
    // ── Apps that take the AirPods ───────────────────────────────────────────
    HMENU appsMenu = CreatePopupMenu();
    {
        const UINT info = MF_BYPOSITION | MF_STRING | MF_DISABLED | MF_GRAYED;
        int userIdx = 0;
        for (const auto& app : status.triggerApps) {
            const wchar_t* when = app.call ? L"when it opens the mic" : L"when it starts playing";
            if (app.builtIn) {
                const std::wstring label = app.exe + L": " + (app.call ? L"calls" : L"playback") + L" (built in)";
                InsertMenuW(appsMenu, static_cast<UINT>(-1), info, 0u, label.c_str());
            } else if (userIdx < kMaxMenuApps) {
                const std::wstring label = L"Remove " + app.exe + L" (" + when + L")";
                InsertMenuW(appsMenu, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING,
                    static_cast<UINT_PTR>(kCmdRemoveTriggerApp + userIdx), label.c_str());
                ++userIdx;
            }
        }
        InsertMenuW(appsMenu, static_cast<UINT>(-1), MF_BYPOSITION | MF_SEPARATOR, 0u, nullptr);
        InsertMenuW(appsMenu, static_cast<UINT>(-1), info, 0u, L"Add an app that used sound since Propods started:");
        if (status.audioApps.empty()) {
            InsertMenuW(appsMenu, static_cast<UINT>(-1), info, 0u,
                L"    (none yet: use the app's sound or mic, then open this menu again)");
        }
        for (int i = 0; i < (int)status.audioApps.size() && i < kMaxMenuApps; ++i) {
            const auto& app = status.audioApps[i];
            // Offered as a call app only once it has opened the mic: that is
            // the event a call app triggers on.
            if (app.usedMic) {
                const std::wstring label = app.exe + L": take the AirPods when it opens the mic (calls)";
                InsertMenuW(appsMenu, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING,
                    static_cast<UINT_PTR>(kCmdAddCallApp + i), label.c_str());
            }
            const std::wstring label = app.exe + L": take the AirPods when it starts playing";
            InsertMenuW(appsMenu, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING,
                static_cast<UINT_PTR>(kCmdAddMediaApp + i), label.c_str());
        }
    }
    InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_POPUP,
        reinterpret_cast<UINT_PTR>(appsMenu), L"Apps that take the AirPods");

    UINT unmuteFlags = MF_BYPOSITION | MF_STRING;
    if (!status.hasMutedApps) unmuteFlags |= MF_GRAYED | MF_DISABLED;
    InsertMenuW(menu, static_cast<UINT>(-1), unmuteFlags,
        static_cast<UINT_PTR>(kCmdUnmuteApps), L"Unmute apps muted for another device");

    UINT autoStartFlags = MF_BYPOSITION | MF_STRING;
    if (isAutoStartEnabled()) autoStartFlags |= MF_CHECKED;
    InsertMenuW(menu, static_cast<UINT>(-1), autoStartFlags,
        static_cast<UINT_PTR>(kCmdAutoStart), L"Start with Windows");

    InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING,
        static_cast<UINT_PTR>(kCmdOpenTraces), L"Open traces folder");
    InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING,
        static_cast<UINT_PTR>(kCmdSendTraces), L"Send traces to peers");

    InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_SEPARATOR, 0u, nullptr);
    InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING,
        static_cast<UINT_PTR>(kCmdQuit), L"Quit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

void TrayIcon::runMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void TrayIcon::postQuit() {
    PostQuitMessage(0);
}

void TrayIcon::toggleAutoStart() {
    bool nowEnabled = !isAutoStartEnabled();
    setAutoStart(nowEnabled);
    log::info("Auto-start on login: {}", nowEnabled ? "enabled" : "disabled");
}

}
