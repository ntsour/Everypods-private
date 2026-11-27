#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace Propods {

class AirPodsConnector {
public:
    explicit AirPodsConnector(std::uint64_t address);
    ~AirPodsConnector();
    AirPodsConnector(const AirPodsConnector&) = delete;
    AirPodsConnector& operator=(const AirPodsConnector&) = delete;

    void setAddress(std::uint64_t address) { m_address = address; }
    std::uint64_t address() const { return m_address; }

    bool connect();
    bool disconnect();

    // Query Win32 Bluetooth API for the device's actual classic-Bluetooth
    // connection state (true = ACL link up to this PC right now).
    bool isClassicallyConnected();

    // Whether the AirPods' ACL link to this PC is up. While watchConnection()
    // is subscribed for the current address this is the state the last
    // ConnectionStatusChanged event reported (no lookup); otherwise it asks
    // the WinRT BluetoothDevice API.
    bool isClassicallyConnectedWinRT();

    // Subscribes to BluetoothDevice::ConnectionStatusChanged for the current
    // address, replacing any earlier subscription, so nothing has to poll
    // (docs/peer-link-quiet-redesign.md, D3). `onChange` runs on a WinRT
    // thread-pool thread with the new state. Returns false if the AirPods are
    // not paired (then isClassicallyConnectedWinRT() keeps looking up).
    bool watchConnection(std::function<void(bool connected)> onChange);
    // The address watchConnection() last subscribed to (0 if none).
    std::uint64_t watchedAddress() const;
    // Looks the state up afresh, logs if the subscription missed a change, and
    // corrects the cached state. The watcher's once-a-minute safety net.
    bool reconcileConnection();

    // The AirPods' Bluetooth name (cached per address), or L"AirPods".
    std::wstring displayName();

    // True when the AirPods' stereo (not Hands-Free) render endpoint is the
    // default multimedia output and ACTIVE. Reads the default endpoint only:
    // no audio sessions are polled and no stream is opened.
    bool isDefaultRenderDevice();

    // The hands-free link (the AirPods' microphone) next to a working stereo
    // link. Windows sometimes brings the AirPods up with stereo only, e.g. when
    // they reconnect by themselves: the mic endpoint stays "Disconnected" and
    // Teams sees no AirPods mic. One enumeration, no audio sessions opened.
    enum class HandsFree { Unknown, Up, Missing };
    HandsFree handsFreeState();

    // Ask the Bluetooth audio driver to connect the hands-free link only
    // (KSPROPERTY_ONESHOT_RECONNECT on the mic endpoint's filter), the same as
    // Sound settings -> Recording -> right-click -> Connect. Windows briefly
    // reopens the stereo link too (~3 s gap on 2026-11-24). Returns false when
    // no filter accepted it.
    bool reconnectHandsFree();

    // True when at least one non-system audio session is in the Active state on
    // the AirPods render endpoint (i.e. a call, meeting, or other app is actively
    // using the AirPods for audio output right now).
    bool hasActiveAudioSessions();

    // True when at least one active audio session on the AirPods render endpoint
    // is owned by a known real-time communications application (Teams, Zoom,
    // Discord, Slack, WebEx). Used as the gate for rejecting peer-initiated
    // kRequestDisconnect packets: we only protect *calls* from being snatched
    // away mid-conversation, not passive media (YouTube, Spotify) that the user
    // is happy to lose if they're moving the AirPods to another device.
    bool hasActiveCallSessions();

    // Make the AirPods the default audio output (render) and input (capture) device
    // at all roles (Console/Multimedia/Communications).
    //
    // Event-driven: registers a process-wide IMMNotificationClient on first call
    // and arms it for 120s. When the audio system promotes the AirPods endpoint
    // to DEVICE_STATE_ACTIVE (typically a few seconds after the BT classical
    // link is up and A2DP has negotiated), the callback applies the routing
    // and broadcasts WM_WININICHANGE so apps switch over.
    //
    // Returns immediately (no blocking poll). Returns true if either routing
    // applied synchronously (endpoint already ACTIVE) or the notifier is armed
    // for a future apply.
    bool setAsDefaultAudioDevice();

    // Toggle the endpoint notifier's "persistent" mode. When true, the notifier
    // stays armed indefinitely and re-arms after each successful routing — so
    // if AirPods bounce off Windows mid-call (Apple auto-switch, brief blip)
    // and come back, routing reapplies automatically without another
    // setAsDefaultAudioDevice() call. Set this to true while we own the AirPods
    // (OwnershipState::LocalPc) and false when ownership leaves us.
    void setPersistentArm(bool persistent);

private:
    bool lookUpConnectionWinRT();
    struct ConnectionWatch;  // WinRT types stay in the .cpp
    mutable std::mutex m_watchMutex;
    std::shared_ptr<ConnectionWatch> m_watch;

    std::uint64_t m_address;
    std::mutex    m_mutex;          // serialize connect/disconnect to avoid parallel races
    std::mutex    m_nameMutex;      // guards m_name / m_nameForAddress
    std::wstring  m_name;
    std::uint64_t m_nameForAddress{0};
};

}
