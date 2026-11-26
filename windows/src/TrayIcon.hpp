#pragma once

#include <functional>
#include <mutex>
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
    static constexpr int kCmdToggleHold        = 1013;
    static constexpr int kCmdToggleLidOpen     = 1016;
    // Shortcuts for when the automatic routing fails (AirPods on this PC only).
    static constexpr int kCmdConnectMic        = 1014;  // connect the hands-free (mic) link
    static constexpr int kCmdUseAirPodsAudio   = 1015;  // default output + input (and Teams)
    // kCmdRemovePeer+i removes the peer at index i in the StatusInfo::peers list.
    static constexpr int kCmdRemovePeer      = 2000;
    // Trigger apps ("Apps that take the AirPods" submenu). The index is into
    // StatusInfo::audioApps (add) or the user entries of StatusInfo::triggerApps
    // (remove), in the order the menu was built from.
    static constexpr int kMaxMenuApps         = 20;
    static constexpr int kCmdAddCallApp       = 3000;
    static constexpr int kCmdAddMediaApp      = 3100;
    static constexpr int kCmdRemoveTriggerApp = 3200;

    // Per-peer info shown in the menu.
    struct PeerStatus {
        std::uint64_t address{};
        std::wstring  name;
        bool          connected{false};
    };

    // An app that takes the AirPods (built in or added by the user).
    struct TriggerAppStatus {
        std::wstring exe;
        bool         call{false};     // call app (mic) vs media app (playback)
        bool         builtIn{false};
    };
    // An app that used audio since Propods started and can be added.
    struct AudioAppStatus {
        std::wstring exe;
        bool         usedMic{false};
    };

    // Snapshot of app status, fetched just before the menu opens.
    struct StatusInfo {
        bool                    airpodsConnected{false};
        bool                    autoRouteEnabled{true};
        bool                    pauseOnRelease{true};
        bool                    takeOnMediaStart{true};
        bool                    takeOnCall{true};
        bool                    holdPlayback{false};
        bool                    lidOpenConnect{true};
        bool                    hasMutedApps{false};
        std::vector<PeerStatus> peers;
        std::vector<TriggerAppStatus> triggerApps;  // built-ins first
        std::vector<AudioAppStatus>   audioApps;    // at most kMaxMenuApps, not yet listed
    };

    using StatusProvider  = std::function<StatusInfo()>;

    // What the tray icon's colour shows (the icon is an AirPods glyph).
    enum class IconState {
        Here,        // AirPods on this PC: green
        Away,        // on another device / unknown: grey (dimmed, like the Mac menu bar)
        Connecting,  // a handover to this PC is in progress: blue
    };

    TrayIcon();
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool create(HINSTANCE hInstance);
    void setStatus(const std::wstring& text);
    // Safe from any thread. Redraws the icon in the state's colour for the
    // current taskbar theme; keeps the old icon if drawing fails.
    void setIconState(IconState state);
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
    std::mutex      m_iconMutex;      // guards m_ownIcon / m_iconState
    HICON           m_ownIcon{};      // icon we drew (destroyed when replaced)
    IconState       m_iconState{IconState::Away};
    bool            m_iconSet{false};
    static constexpr UINT WM_TRAY = WM_APP + 1;
};

}
