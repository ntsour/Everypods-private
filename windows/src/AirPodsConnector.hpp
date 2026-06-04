#pragma once

#include <cstdint>
#include <mutex>

namespace Propods {

class AirPodsConnector {
public:
    explicit AirPodsConnector(std::uint64_t address);
    AirPodsConnector(const AirPodsConnector&) = delete;
    AirPodsConnector& operator=(const AirPodsConnector&) = delete;

    void setAddress(std::uint64_t address) { m_address = address; }
    std::uint64_t address() const { return m_address; }

    bool connect();
    bool disconnect();

    // Query Win32 Bluetooth API for the device's actual classic-Bluetooth
    // connection state (true = ACL link up to this PC right now).
    bool isClassicallyConnected();

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

    // Last-resort routing for the "managed competitor won't let go" case (e.g.
    // Jabra Direct keeps re-asserting the Jabra as default and Windows won't
    // migrate the live stream to the AirPods). Briefly DISABLES the competing
    // render endpoint(s) — those currently holding a default role or an active
    // session that are not the AirPods. Disabling forces Windows to migrate the
    // active stream onto the current default (AirPods); we re-route + resume,
    // give the stream a moment to establish, then re-enable the competitor.
    // Once the stream is actively on the AirPods, re-enabling is safe: a default
    // change alone never moves an already-active stream. Blocks ~3-4s; call from
    // the detached takeover thread only. Crash-safe: disabled endpoint IDs are
    // persisted to disk and restored on next startup (see
    // restoreDisabledEndpointsOnStartup). No-op if no competitor is found.
    void evictCompetitorsToClaimRoute();

    // Re-enable any audio endpoints that a previous run disabled via
    // evictCompetitorsToClaimRoute() but never restored (e.g. the process
    // crashed mid-eviction). Reads the persisted ID list, re-enables each, and
    // clears the list. Call once at startup before normal operation.
    static void restoreDisabledEndpointsOnStartup();

private:
    std::uint64_t m_address;
    std::mutex m_mutex;  // serialize connect/disconnect to avoid parallel races
};

}
