#pragma once

#include <functional>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

namespace Propods {

class TrayIcon {
public:
    using MenuHandler = std::function<void(int commandId)>;

    static constexpr int kCmdQuit            = 1001;
    static constexpr int kCmdPairAndroid     = 1002;
    static constexpr int kCmdPairAirPods     = 1003;
    static constexpr int kCmdAutoStart       = 1004;
    static constexpr int kCmdConnectAirPods  = 1005;
    static constexpr int kCmdToggleAutoRoute = 1006;
    static constexpr int kCmdOpenTraces      = 1007;
    static constexpr int kCmdSendTraces      = 1008;
    static constexpr int kCmdTogglePause     = 1009;
    static constexpr int kCmdUnmuteApps      = 1010;
    static constexpr int kCmdToggleTakeOnMedia = 1011;
    static constexpr int kCmdToggleTakeOnCall  = 1012;
    // kCmdRemovePeer+i removes the peer at index i in the StatusInfo::peers list.
    static constexpr int kCmdRemovePeer      = 2000;

    // Per-peer info shown in the menu.
    struct PeerStatus {
        std::uint64_t address{};
        std::wstring  name;
        bool          connected{false};
    };

    // Snapshot of app status, fetched just before the menu opens.
    struct StatusInfo {
        bool                    airpodsConnected{false};
        bool                    autoRouteEnabled{true};
        bool                    pauseOnRelease{true};
        bool                    takeOnMediaStart{true};
        bool                    takeOnCall{true};
        bool                    hasMutedApps{false};
        std::vector<PeerStatus> peers;
    };

    using StatusProvider  = std::function<StatusInfo()>;

    TrayIcon();
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool create(HINSTANCE hInstance);
    void setStatus(const std::wstring& text);
    // Windows notification from the tray icon (safe to call from any thread).
    void notify(const std::wstring& title, const std::wstring& text);
    void setMenuHandler(MenuHandler h)       { m_menuHandler    = std::move(h); }
    void setStatusProvider(StatusProvider p) { m_statusProvider = std::move(p); }

    void runMessageLoop();
    void postQuit();
    void toggleAutoStart();

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void showContextMenu(HWND hwnd);

    HWND m_hwnd{};
    NOTIFYICONDATAW m_nid{};
    std::wstring m_statusText{L"Propods"};
    MenuHandler     m_menuHandler;
    StatusProvider  m_statusProvider;
    static constexpr UINT WM_TRAY = WM_APP + 1;
};

}
