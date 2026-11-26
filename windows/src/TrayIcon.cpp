#include "TrayIcon.hpp"

#include "AirPodsIcon.hpp"
#include "Logger.hpp"

#include <algorithm>

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

namespace {

// "takes the AirPods when it opens the mic; keeps the AirPods while it uses the mic"
std::wstring appModeText(const TrayIcon::TriggerAppStatus& app) {
    if (app.builtIn) return L"built in";
    if (app.neverTake) return L"never takes the AirPods";
    std::wstring s;
    auto add = [&s](const wchar_t* t) { if (!s.empty()) s += L"; "; s += t; };
    if (app.takeOnMic)  add(L"takes the AirPods when it opens the mic");
    if (app.takeOnPlay) add(L"takes the AirPods when it starts playing");
    if (app.keepOnMic)  add(L"keeps the AirPods while it uses the mic");
    return s.empty() ? L"default" : s;
}

}  // namespace

// Default, separator, the three settings, separator, Never. Built-ins show
// their three settings greyed, with no Default or Never.
HMENU TrayIcon::buildAppSubmenu(const TriggerAppStatus& app, int index) {
    HMENU sub = CreatePopupMenu();
    const int base = kCmdAppRule + index * kAppChoices;
    const bool isDefault = !app.takeOnMic && !app.takeOnPlay && !app.keepOnMic && !app.neverTake;
    auto item = [&](int choice, bool checked, const wchar_t* text) {
        UINT flags = MF_BYPOSITION | MF_STRING;
        if (checked) flags |= MF_CHECKED;
        if (app.builtIn) flags |= MF_DISABLED | MF_GRAYED;
        InsertMenuW(sub, static_cast<UINT>(-1), flags, static_cast<UINT_PTR>(base + choice), text);
    };
    auto separator = [&] { InsertMenuW(sub, static_cast<UINT>(-1), MF_BYPOSITION | MF_SEPARATOR, 0u, nullptr); };
    if (!app.builtIn) {
        item(0, isDefault, L"Default: take them when it plays in the Windows media controls");
        separator();
    }
    item(1, app.takeOnMic,  L"Take the AirPods when it opens the mic");
    item(2, app.takeOnPlay, L"Take the AirPods when it starts playing");
    item(3, app.keepOnMic,  L"Keep the AirPods while it uses the mic");
    if (!app.builtIn) {
        separator();
        item(4, app.neverTake, L"Never take the AirPods");
    }
    return sub;
}

bool TrayIcon::keepsMenuOpen(int cmd) {
    switch (cmd) {
        case kCmdToggleAutoRoute: case kCmdTogglePause: case kCmdToggleTakeOnMedia:
        case kCmdToggleTakeOnCall: case kCmdToggleHold: case kCmdToggleLidOpen:
        case kCmdAutoStart:
            return true;
        default:
            return isAppChoice(cmd);
    }
}

void TrayIcon::showContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    // After a setting toggle the menu opens again at the same spot; after an
    // app choice, that app's submenu opens on its own so the next tick is one
    // click away. Escape or a click outside ends it.
    std::wstring reopenApp;
    for (int round = 0; round < 100; ++round) {
        // Fetch live status just before the menu opens.
        StatusInfo status;
        if (m_statusProvider) status = m_statusProvider();

        HMENU menu = nullptr;
        if (!reopenApp.empty()) {
            const int n = (std::min)((int)status.apps.size(), kMaxMenuApps);
            for (int i = 0; i < n && !menu; ++i)
                if (status.apps[i].exe == reopenApp) menu = buildAppSubmenu(status.apps[i], i);
        }
        if (!menu) menu = buildMenu(status);

        SetForegroundWindow(hwnd);
        const int cmd = static_cast<int>(TrackPopupMenu(menu,
            TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr));
        PostMessageW(hwnd, WM_NULL, 0, 0);  // lets the next menu open and close normally
        DestroyMenu(menu);
        if (cmd == 0) return;

        reopenApp.clear();
        if (isAppChoice(cmd)) {
            const int idx = (cmd - kCmdAppRule) / kAppChoices;
            if (idx < (int)status.apps.size()) reopenApp = status.apps[idx].exe;
        }
        if (m_menuHandler) m_menuHandler(cmd);
        if (!keepsMenuOpen(cmd)) return;
    }
}

HMENU TrayIcon::buildMenu(const StatusInfo& status) {
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
    // ── Apps that take or keep the AirPods ──────────────────────────────────
    // Every app that used sound or the mic (remembered), every app the user
    // set, and the built-ins, sorted by name; each line shows its mode.
    HMENU appsMenu = CreatePopupMenu();
    {
        const int n = (std::min)((int)status.apps.size(), kMaxMenuApps);
        for (int i = 0; i < n; ++i) {
            const auto& app = status.apps[i];
            const std::wstring label = app.exe + L": " + appModeText(app);
            InsertMenuW(appsMenu, static_cast<UINT>(-1), MF_BYPOSITION | MF_POPUP,
                reinterpret_cast<UINT_PTR>(buildAppSubmenu(app, i)), label.c_str());
        }
        InsertMenuW(appsMenu, static_cast<UINT>(-1), MF_BYPOSITION | MF_SEPARATOR, 0u, nullptr);
        InsertMenuW(appsMenu, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING | MF_DISABLED | MF_GRAYED, 0u,
            L"An app shows up here once it has used sound or the mic");
    }
    InsertMenuW(menu, static_cast<UINT>(-1), MF_BYPOSITION | MF_POPUP,
        reinterpret_cast<UINT_PTR>(appsMenu), L"Apps that take or keep the AirPods");

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

    return menu;
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
