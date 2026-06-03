#include "HandoverController.hpp"

#include "Logger.hpp"
#include "crossdevice_protocol.hpp"

#include <array>
#include <thread>
#include <windows.h>

namespace Propods {

HandoverController::HandoverController(
    PeerRegistry& peers,
    AirPodsConnector& airpods,
    MediaPlaybackWatcher& media)
    : m_peers(peers), m_airpods(airpods), m_media(media)
{
    startAudioWatcher();
}

HandoverController::~HandoverController() {
    m_watcherRunning.store(false);
    if (m_watcherThread.joinable()) m_watcherThread.join();
}

void HandoverController::startAudioWatcher() {
    m_watcherRunning.store(true);
    m_watcherThread = std::thread([this]() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const std::exception& e) {
            log::warn("Audio watcher: failed to init WinRT apartment: {}", e.what());
            return;  // Exit thread, but don't crash the app
        }

        // Hysteresis: gain "active" instantly (we want peer-protection ASAP),
        // but require ≥2 consecutive idle polls (≥1 s at 500 ms cadence) before
        // dropping to "idle". A single-poll WASAPI/BT flicker during Teams route
        // renegotiation would otherwise tell Android "go ahead, grab the AirPods"
        // for a few hundred ms — long enough for Android's takeover to fire.
        bool lastActive = false;     // last state broadcast to peers
        int  idleStreak = 0;         // # consecutive polls observed idle while broadcast=active
        constexpr int kIdleStreakThreshold = 2;
        // Initialised to actual state so the first poll doesn't produce a synthetic edge.
        bool lastBtConnected = m_airpods.isClassicallyConnected();
        while (m_watcherRunning.load()) {
            try {
                // Resolve Unknown state if peers are now connected.
                // Guard the packet send on setState's return value: if onPeerConnectionChanged
                // raced us here and already resolved Unknown, setState returns false and we
                // skip the duplicate send.
                if (m_state.load() == OwnershipState::Unknown && m_peers.isAnyConnected()) {
                    bool airpodsHere = m_airpods.isClassicallyConnected();
                    bool changed = setState(airpodsHere ? OwnershipState::LocalPc
                                                        : OwnershipState::RemoteAndroid);
                    if (changed) {
                        log::handover("OUT     {} -> peer (periodic sync on Unknown state)",
                            airpodsHere ? "kAirPodsConnected" : "kAirPodsDisconnected");
                        m_peers.sendPacket(airpodsHere
                            ? crossdevice::kAirPodsConnected
                            : crossdevice::kAirPodsDisconnected);
                    }
                }

                // BT-edge observer: catches connect/disconnect events that bypass the
                // CrossDevice protocol (Apple auto-switch, manual connect, phone grabs via BT
                // before RFCOMM is up). Runs every 500 ms poll — fast enough for the handover
                // feel while cheap enough not to stress the BT stack.
                bool btConnected = m_airpods.isClassicallyConnected();
                if (btConnected && !lastBtConnected) {
                    // Rising edge: AirPods just appeared on Windows (X1, X3, C2 scenarios).
                    // Reset audio state: the classical BT link is up but A2DP hasn't negotiated
                    // yet, so there are no audio sessions. Carrying stale lastActive=true into
                    // this window makes the watcher misread the normal A2DP startup gap as
                    // "audio was active, now gone -> phone stole them" and fire a false RECLAIM.
                    // Starting fresh means the watcher waits to observe what actually happens.
                    lastActive = false;
                    idleStreak = 0;
                    // Don't call setAsDefaultAudioDevice here — avoids stealing audio routing
                    // when the user manually connected for another purpose (e.g., firmware update).
                    if (setState(OwnershipState::LocalPc)) {
                        log::handover("BT      AirPods connected (BT rising edge) — claiming ownership");
                        if (m_peers.isAnyConnected()) {
                            log::handover("OUT     kAirPodsConnected -> all peers (BT edge)");
                            m_peers.sendPacket(crossdevice::kAirPodsConnected);
                        }
                    }
                } else if (!btConnected && lastBtConnected) {
                    // Falling edge: AirPods left Windows. Only act when we thought we owned them.
                    if (m_state.load() == OwnershipState::LocalPc) {
                        // Unified pause-and-release. We previously had two branches:
                        //   - lastActive=true  -> RECLAIM (try to grab AirPods back)
                        //   - lastActive=false -> pause local media, release ownership
                        //
                        // Experience showed the RECLAIM was *always* wrong in practice:
                        //   - When the user intentionally moved AirPods to the phone, RECLAIM
                        //     fought the user's choice and snatched them back.
                        //   - When AirPods went into the case / died / were stolen by Apple's
                        //     auto-switch, RECLAIM couldn't find them anyway and spammed
                        //     pointless retries.
                        //   - For the original target scenario (mid-Teams-call phone grab),
                        //     pausing Windows media is actually correct: Teams' own multi-
                        //     device handover takes the call to the phone, and Windows
                        //     shouldn't fight that.
                        //
                        // Brief-bounce protection (AirPods drop off Windows for a second and
                        // come back) is now handled at the audio-endpoint layer by the
                        // IMMNotificationClient's persistent-arm mode: while we own the
                        // AirPods, any UNPLUGGED->ACTIVE re-applies routing automatically.
                        //
                        // So: always pause local media and release ownership on BT falling
                        // edge while we held LocalPc.
                        const char* reason = lastActive
                            ? "BT      AirPods disconnected mid-active-audio — pausing local media and releasing (no fight-back)"
                            : "BT      AirPods disconnected (BT falling edge, idle) — releasing";

                        // Set the anti-pingpong timestamp FIRST, before any potentially-blocking
                        // operation. Mirrors the fix in onIncomingPacket's release handlers. The
                        // pause calls below can take 10-50 ms each (GSMTC, SendInput), and the
                        // PlaybackInfoChanged callback runs on a separate GSMTC thread — without
                        // this, Chrome auto-resuming when audio falls back to PC speakers can
                        // fire onMediaPlayingChanged(true) before m_lastLostOwnership is set,
                        // and anti-pingpong fails to suppress the counter-takeover.
                        m_lastLostOwnership.store(tpToNs(std::chrono::steady_clock::now()));

                        m_media.tryPauseActive();
                        m_media.tryPauseAllSessions();
                        m_media.tryPauseViaMediaKey();
                        m_media.tryPauseAllBrowserWindows();
                        if (setState(OwnershipState::RemoteAndroid)) {
                            log::handover("{}", reason);
                            if (m_peers.isAnyConnected()) {
                                log::handover("OUT     kAirPodsDisconnected -> all peers (BT edge)");
                                m_peers.sendPacket(crossdevice::kAirPodsDisconnected);
                            }
                        }
                    }
                    // state != LocalPc: RequestDisconnect / RequestHandover already updated state
                    // before the BT stack confirmed the drop — expected, no-op.
                }
                lastBtConnected = btConnected;

                // Only meaningful when AirPods are connected to this PC. We use the
                // *call-specific* check (Teams/Zoom/Discord/Slack/WebEx) rather than
                // hasActiveAudioSessions() so that kWindowsAudioActive is a meaningful
                // "do not interrupt" signal: peers should hold off from a takeover
                // when we're in a call, but should NOT hold off just because we have
                // music playing — the user is free to move the AirPods to their phone
                // in that case. Symmetric with the R1 narrowing of the local REJECT
                // gate (HandoverController.cpp:378). Without this, a peer that gates
                // its own takeover on the received kWindowsAudioActive signal would
                // refuse to migrate the AirPods during ordinary YouTube/Spotify
                // playback.
                bool active = btConnected && m_airpods.hasActiveCallSessions();

                if (active && !lastActive) {
                    // Rising edge: broadcast ACTIVE immediately.
                    lastActive = true;
                    idleStreak = 0;
                    log::handover("AUDIO   ACTIVE — AirPods have an active call session");
                    if (m_peers.isAnyConnected()) {
                        m_peers.sendPacket(crossdevice::kWindowsAudioActive);
                    }
                } else if (!active && lastActive) {
                    // Idle observed while we still consider ourselves active — count it.
                    ++idleStreak;
                    if (idleStreak >= kIdleStreakThreshold) {
                        // Sustained idle: broadcast IDLE as an informational signal to peers.
                        // We do NOT release ownership here — Windows holds the AirPods until
                        // another device actively claims them (RequestDisconnect / RequestHandover
                        // over RFCOMM, or a BT falling edge from a direct phone grab).
                        lastActive = false;
                        idleStreak = 0;
                        log::handover("AUDIO   IDLE   — AirPods call session ended");
                        if (m_peers.isAnyConnected()) {
                            m_peers.sendPacket(crossdevice::kWindowsAudioIdle);
                        }
                    } else {
                        log::handover("AUDIO   transient blip — holding active state (streak {}/{})",
                                      idleStreak, kIdleStreakThreshold);
                    }
                } else if (active && lastActive) {
                    // Recovered before the threshold — reset the streak silently.
                    idleStreak = 0;
                }
                // !active && !lastActive: steady idle. Nothing to do — we hold ownership
                // until an explicit external event takes it.
            } catch (const std::exception& e) {
                log::warn("Audio watcher: exception during poll: {}", e.what());
                // Continue polling, don't crash
            }

            // Sliced sleep so shutdown is responsive (50 ms × 10 = 500 ms poll interval).
            for (int i = 0; i < 10 && m_watcherRunning.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });
}

bool HandoverController::setState(OwnershipState s) {
    auto previous = m_state.exchange(s);
    if (previous != s) {
        const char* label = (s == OwnershipState::LocalPc)     ? "LocalPc"
                          : (s == OwnershipState::RemoteAndroid) ? "RemoteAndroid"
                          :                                        "Unknown";
        log::handover("STATE -> {}", label);
        if (m_onStateChanged) m_onStateChanged(s);

        // Persistent-arm the endpoint notifier whenever we hold ownership. While
        // we're LocalPc, AirPods bouncing off Windows briefly (Apple auto-switch,
        // brief BT blip) and coming back will re-route automatically without a
        // fresh setAsDefaultAudioDevice() call. When ownership leaves us, drop
        // the persistent flag so we don't fight a peer that is intentionally
        // taking the AirPods.
        m_airpods.setPersistentArm(s == OwnershipState::LocalPc);

        // Claiming AirPods (by any means: BT rising edge, connect-thread success,
        // manual tray "Connect AirPods now") resets the voluntary-release gate so
        // future call/media triggers are not suppressed.
        if (s == OwnershipState::LocalPc) {
            m_voluntarilyReleased.store(false);
            // Clear the W4 app-suppress window so the next voluntary release starts fresh.
            std::scoped_lock lk{m_voluntaryReleaseAppMutex};
            m_voluntaryReleaseAppId.clear();
        }

        return true;
    }
    return false;
}

bool HandoverController::withinDebounceWindow() {
    std::scoped_lock lk{m_debounceMutex};
    auto now = std::chrono::steady_clock::now();
    if (now - m_lastAction < kDebounce) return true;
    m_lastAction = now;
    return false;
}

void HandoverController::onMediaPlayingChanged(bool playing) {
    if (!playing) {
        log::debug("Media stopped — keeping current ownership");
        // W1 fix: only clear the voluntary-release gate if we are past the immediate
        // post-release window (5 s). Within that window the stop was caused by our own
        // pause sequence during handover, not by the user explicitly stopping — clearing
        // here would let a Teams re-trigger fire immediately after the handover completes.
        auto sinceRelease = std::chrono::steady_clock::now() - tpFromNs(m_lastVoluntaryReleaseAt.load());
        if (sinceRelease > std::chrono::milliseconds(5000)) {
            if (m_voluntarilyReleased.exchange(false))
                log::handover("STATE   m_voluntarilyReleased cleared (media stopped)");
        }
        return;
    }
    if (withinDebounceWindow()) {
        log::debug("Debounced media-start event");
        return;
    }

    const bool actuallyConnected = m_airpods.isClassicallyConnected();
    const bool stateLocal = (m_state.load() == OwnershipState::LocalPc);
    log::debug("Media start: state={} airpodsConnected={}",
        stateLocal ? "LocalPc" : "remote/unknown",
        actuallyConnected);

    if (actuallyConnected) {
        if (stateLocal) {
            log::handover("SKIP    AirPods already on this PC — no action needed");
            return;
        }
        // AirPods are at the BT layer but state is remote/unknown — this happens on
        // startup when AirPods are already connected (Apple auto-reconnect, previous
        // session) before the RFCOMM peer channel comes up, or when AirPods
        // auto-switched to Windows without a protocol handover. Running connect()
        // here would call BluetoothSetServiceState(A2DP, ENABLE) on an already-live
        // link, which tears down and re-establishes A2DP and causes a ~15s UNPLUGGED
        // window during which resumed audio goes to PC speakers with no sound.
        // Instead: claim ownership directly and re-route without touching the link.
        log::handover("CLAIM   AirPods already BT-connected (state was remote/unknown) — "
                      "claiming ownership, skipping connect()");
        setState(OwnershipState::LocalPc);
        m_lastLocalTakeover.store(tpToNs(std::chrono::steady_clock::now()));
        if (m_peers.isAnyConnected()) {
            log::handover("OUT     kAirPodsConnected -> all peers");
            m_peers.sendPacket(crossdevice::kAirPodsConnected);
        }
        if (m_autoRoute.load()) m_airpods.setAsDefaultAudioDevice();
        return;
    }

    // AirPods are NOT classically connected to this PC. Before attempting a BT
    // takeover, consult the BLE proximity probe: if the AirPods appear to be in
    // their case / not nearby, do nothing. Attempting connect() here would poke
    // the BT stack and, on success, hijack audio away from whatever device the
    // user is currently on (Jabra/Dell headset, speakers). The probe fails open
    // (returns true) if BLE is unavailable, so handover is never silently broken.
    // The manual "Connect AirPods now" tray command bypasses this entirely.
    if (m_availabilityProbe && !m_availabilityProbe()) {
        log::handover("SKIP    AirPods appear to be in the case / not nearby — "
                      "not initiating takeover (avoids interfering with current audio device)");
        return;
    }

    // Anti-pingpong: if we just lost ownership to Android (or a prior takeover
    // attempt just failed), don't immediately take back.
    auto sinceLost = std::chrono::steady_clock::now() - tpFromNs(m_lastLostOwnership.load());
    if (sinceLost < std::chrono::milliseconds(3000)) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(sinceLost).count();
        log::handover("SKIP    Anti-pingpong: lost ownership {}ms ago — not reclaiming", ms);
        return;
    }

    // W4 fix: suppress re-triggers from the same app within 30 s of a voluntary release.
    // Prevents YouTube ads and Chrome auto-resume from stealing AirPods back after the
    // user explicitly handed them to a phone.
    {
        std::scoped_lock lk{m_voluntaryReleaseAppMutex};
        auto sinceRelease = std::chrono::steady_clock::now() - tpFromNs(m_lastVoluntaryReleaseAt.load());
        if (!m_voluntaryReleaseAppId.empty() && sinceRelease < kVoluntaryReleaseSuppressWindow) {
            const std::string curApp = m_media.currentAppId();
            if (curApp == m_voluntaryReleaseAppId) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(sinceRelease).count();
                log::handover("SKIP    Voluntary-release suppression: app '{}' re-triggered {}ms after release",
                              curApp, ms);
                return;
            }
        }
    }

    // In-flight guard: one takeover attempt at a time. Without this, the connect
    // thread's tryPlayActive() on failure resumes media, GSMTC fires playing=true,
    // and onMediaPlayingChanged immediately spawns another thread — flooding peers
    // with REQUEST_DISCONNECT at the connect-backoff cadence (~1/s with 3 peers).
    if (m_takeoverInFlight.exchange(true)) {
        log::handover("SKIP    Takeover already in flight — dropping trigger");
        return;
    }

    const std::string appId = m_media.currentAppId();
    log::handover("TRIGGER Media/call active (app={}) — requesting handover from Android",
                  appId.empty() ? "unknown" : appId);
    // Pause local media so audio doesn't leak through PC speakers during the
    // ~1-2s while AirPods are migrating. We'll resume it once we've claimed them.
    const bool paused = m_media.tryPauseActive();

    // Capture the kickoff time BEFORE sending kRequestDisconnect so the takeover
    // thread can distinguish a "kAirPodsDisconnected from peer" arriving in response
    // to *this* request from earlier stale ones.
    const auto kickoffTime = std::chrono::steady_clock::now();
    const bool peerConnected = m_peers.isAnyConnected();
    if (peerConnected) {
        log::handover("OUT     kRequestDisconnect -> all peers");
        m_peers.sendPacket(crossdevice::kRequestDisconnect);
    } else {
        log::handover("WARN    No peers connected — attempting BT connect without coordination");
    }

    // Dispatch all blocking work (sleep + BT connect + audio endpoint poll) to a
    // detached thread. The WinRT media-callback thread must not be blocked: the
    // runtime will terminate a callback thread that doesn't return promptly, causing
    // a crash.
    //
    // Kickoff: instead of a fixed 1500 ms wait, listen on m_peerAckCv for the peer
    // to confirm kAirPodsDisconnected. The Pixel app typically acks within ~100 ms.
    // We connect ~300 ms after ack to give the AirPods time to actually release at
    // the BT layer (acking the RFCOMM packet ≠ A2DP fully released). If no peer ack
    // arrives within 1500 ms, fall through to the timer-based path.
    //
    // Subsequent retries use a backoff (1500ms, 3000ms) — kept short because Xiaomi
    // MIUI and similar slow vendors can take 2-5 s to actually release A2DP after
    // the RFCOMM ack. The first attempt is the fast path; retries cover stragglers.
    std::thread([this, paused, kickoffTime, peerConnected]() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        // Phase 1: event-driven kickoff for the first attempt.
        constexpr auto kMaxAckWait     = std::chrono::milliseconds(1500);
        constexpr auto kPostAckGrace   = std::chrono::milliseconds(300);
        const auto ackDeadline = kickoffTime + kMaxAckWait;

        bool gotPeerAck = false;
        if (peerConnected) {
            std::unique_lock<std::mutex> lk(m_peerAckMtx);
            gotPeerAck = m_peerAckCv.wait_until(lk, ackDeadline, [this, &kickoffTime]() {
                return tpFromNs(m_lastPeerAckAt.load()) >= kickoffTime;
            });
        }

        if (gotPeerAck) {
            auto ackedAfter = std::chrono::duration_cast<std::chrono::milliseconds>(
                tpFromNs(m_lastPeerAckAt.load()) - kickoffTime).count();
            log::handover("ACTION  Peer acked kAirPodsDisconnected after {}ms — grace {}ms then BT connect",
                ackedAfter, kPostAckGrace.count());
            std::this_thread::sleep_for(kPostAckGrace);
        } else {
            auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - kickoffTime).count();
            log::handover("ACTION  No peer ack within {}ms — proceeding with BT connect anyway",
                waited);
        }

        // First attempt
        log::handover("ACTION  BT connect attempt 1/3");
        bool connected = m_airpods.connect();
        if (!connected) {
            log::handover("RETRY   BT connect attempt 1 failed — backing off");
            constexpr std::array<int, 2> kBackoffsMs{1500, 3000};
            for (size_t i = 0; i < kBackoffsMs.size(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kBackoffsMs[i]));
                log::handover("ACTION  BT connect attempt {}/3 (after {} ms wait)",
                              i + 2, kBackoffsMs[i]);
                if (m_airpods.connect()) {
                    connected = true;
                    break;
                }
                log::handover("RETRY   BT connect attempt {} failed — backing off", i + 2);
            }
        }
        if (connected) {
            m_takeoverInFlight.store(false);
            setState(OwnershipState::LocalPc);
            m_lastLocalTakeover.store(tpToNs(std::chrono::steady_clock::now()));
            if (m_peers.isAnyConnected()) {
                log::handover("OUT     kAirPodsConnected -> all peers");
                m_peers.sendPacket(crossdevice::kAirPodsConnected);
            }
            // Make AirPods the default audio render + capture device (if auto-route is on).
            if (m_autoRoute.load()) m_airpods.setAsDefaultAudioDevice();

            // Resume media now that AirPods are the active route. Always attempt this —
            // not conditional on `paused` — because tryPauseActive() can return false when
            // GSMTC hasn't registered the session yet (e.g. YouTube first play), in which
            // case the browser pause commands still silenced playback but paused=false and
            // tryPlayActive() would be skipped, leaving AirPods connected but silent.
            // Small delay lets the audio stack settle on the new endpoint first.
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            m_media.tryPlayActive();

            // Deferred retry: device-management software (Jabra Direct, Poly Lens,
            // enterprise audio policies) often reasserts a managed headset as the
            // audio default a few seconds after a change. Re-assert AirPods after
            // 3 s to win that race. Also re-writes Teams' cmd_settings.json so any
            // call that the user answers in the interim picks up AirPods.
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
            if (m_autoRoute.load() && m_airpods.isClassicallyConnected()) {
                log::handover("ACTION  Re-asserting AirPods as default audio (3s retry)");
                m_airpods.setAsDefaultAudioDevice();
            }
        } else {
            log::handover("FAIL    BT connect failed after 3 attempts — AirPods not acquired");
            // Set m_lastLostOwnership BEFORE tryPlayActive so the media-resume
            // event (GSMTC playing=true fired by tryPlayActive) hits the 3s
            // anti-pingpong window and doesn't immediately re-trigger a new
            // takeover attempt — which would flood peers with REQUEST_DISCONNECT.
            m_lastLostOwnership.store(tpToNs(std::chrono::steady_clock::now()));
            m_takeoverInFlight.store(false);
            if (paused) {
                // Takeover failed — resume on whatever route we have so we don't leave
                // the user with paused media for no reason.
                m_media.tryPlayActive();
            }
            // Arm the endpoint notifier even on failure.  If AirPods become
            // available within the next 120 s (user takes them out of case,
            // disconnects from phone, etc.), the IMMNotificationClient fires
            // and routes audio to AirPods automatically — no second Teams
            // trigger needed.  setState(LocalPc) is intentionally NOT called
            // here; the audio watcher's BT-rising-edge poll will do that
            // within 500 ms once the link actually comes up.
            if (m_autoRoute.load()) {
                log::handover("INFO    Arming endpoint notifier (120s window) — "
                              "routing will fire if AirPods appear while Teams call is active");
                m_airpods.setAsDefaultAudioDevice();
            }
        }
    }).detach();
}

void HandoverController::onIncomingPacket(std::span<const std::uint8_t> data) {
    using namespace crossdevice;
    auto kind = classify(data);
    switch (kind) {
        case Incoming::RequestDisconnect: {
            // Only the holder can release AirPods. If we're not in LocalPc state,
            // reply with kAirPodsDisconnected to confirm our state to the peer and
            // do nothing else. Without this guard, Android broadcasts kRequestDisconnect
            // to all peers including non-holders, which would incorrectly reset
            // m_lastLostOwnership (arming the 3-second anti-pingpong window) and flood
            // the mesh with spurious kAirPodsDisconnected packets.
            if (m_state.load() != OwnershipState::LocalPc) {
                log::handover("IN      kRequestDisconnect — IGNORED (not the holder, state={}); confirming kAirPodsDisconnected",
                    m_state.load() == OwnershipState::RemoteAndroid ? "RemoteAndroid" : "Unknown");
                if (m_peers.isAnyConnected())
                    m_peers.sendPacket(kAirPodsDisconnected);
                break;
            }
            // Reject takeover attempts that happen within ~3s of our own takeover.
            // Android's MediaController fires takeover whenever media is "active",
            // which is often true right after we ourselves grabbed the AirPods.
            auto sinceTakeover = std::chrono::steady_clock::now() - tpFromNs(m_lastLocalTakeover.load());
            if (sinceTakeover < std::chrono::milliseconds(3000)) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(sinceTakeover).count();
                log::handover("IN      kRequestDisconnect — REJECTED (anti-pingpong, {}ms since takeover)", ms);
                if (m_peers.isAnyConnected()) {
                    m_peers.sendPacket(kAirPodsConnected);
                }
                break;
            }
            // Requester always wins: every peer takeover is an explicit user action on
            // the requesting device (media start / call answered), so the holder yields
            // unconditionally. The prior holder-side call veto was removed for symmetry
            // with Android's Phase 3 design (see docs/handover.md "Requester Always Wins"
            // and docs/windows-multipeer.md). The anti-pingpong reject above is kept —
            // it guards against our own just-completed takeover bouncing back, not a
            // holder veto.
            log::handover("IN      kRequestDisconnect — ACCEPTED, releasing AirPods to Android");

            // Mark voluntary release so onCallActive() suppresses Teams/WASAPI re-triggers
            // for the duration of the current call (until session expires or media stops).
            m_voluntarilyReleased.store(true);
            log::handover("STATE   m_voluntarilyReleased = true (voluntary release)");

            // W4 fix: record which app was playing so we can suppress its re-trigger for 30 s.
            // Deferred pause retry fires 2 s later to catch YouTube ads that start playing
            // after the initial pause sequence (which only runs at the moment of release).
            {
                std::scoped_lock lk{m_voluntaryReleaseAppMutex};
                m_voluntaryReleaseAppId = m_media.currentAppId();
            }
            m_lastVoluntaryReleaseAt.store(tpToNs(std::chrono::steady_clock::now()));
            std::thread([this]() {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                if (m_state.load() != OwnershipState::LocalPc) {
                    log::handover("ACTION  Deferred pause retry (2s) — suppressing app resume after release");
                    m_media.tryPauseActive();
                    m_media.tryPauseAllSessions();
                    m_media.tryPauseViaMediaKey();
                    m_media.tryPauseAllBrowserWindows();
                }
            }).detach();

            // Record "lost ownership" NOW, before any potentially-blocking operation.
            // Chrome and other apps auto-resume playback within ~30-50ms when the
            // default audio device changes (which our disconnect triggers). If we
            // delay this store until after disconnect()/setState() (~50-150ms later),
            // the media-start callback that fires during those ~50ms sees a stale
            // m_lastLostOwnership, skips the anti-pingpong check, and triggers a
            // counter-takeover that fights the peer that just legitimately claimed
            // the AirPods. Setting it first makes the anti-pingpong window cover
            // the entire release sequence.
            m_lastLostOwnership.store(tpToNs(std::chrono::steady_clock::now()));

            // Pause all Windows media before the AirPods leave so audio doesn't route
            // to PC speakers, and so the media-start callback doesn't fire and
            // immediately try to reclaim the AirPods.
            // Fire all three methods regardless of earlier successes: a GSMTC pause
            // of Spotify does not stop VLC (non-GSMTC), and the media key catches
            // anything else (browsers, etc.).
            m_media.tryPauseActive();        // GSMTC: pause the focused session
            m_media.tryPauseAllSessions();   // GSMTC: pause any background sessions
            m_media.tryPauseViaMediaKey();   // Media key: VLC, browsers, non-GSMTC apps
            m_media.tryPauseAllBrowserWindows();  // multi-window Chrome/Edge/Firefox
            m_airpods.disconnect();
            setState(OwnershipState::RemoteAndroid);
            if (m_peers.isAnyConnected()) {
                log::handover("OUT     kAirPodsDisconnected -> all peers");
                m_peers.sendPacket(kAirPodsDisconnected);
            }
            break;
        }

        case Incoming::RequestHandover: {
            // Only yield if we are currently the holder.
            if (m_state.load() != OwnershipState::LocalPc) {
                log::handover("IN      kRequestHandover — IGNORED (not the holder, state={}); confirming kAirPodsDisconnected",
                    m_state.load() == OwnershipState::RemoteAndroid ? "RemoteAndroid" : "Unknown");
                if (m_peers.isAnyConnected())
                    m_peers.sendPacket(kAirPodsDisconnected);
                break;
            }
            // Call-priority handover request from peer. Unlike RequestDisconnect,
            // this is never rejected — the peer is taking AirPods for a call, which
            // is peak user attention. Even mid-Teams-meeting on Windows, we yield.
            // The user's principle: calls always win, on whichever device they're on.
            log::handover("IN      kRequestHandover — ACCEPTED unconditionally (call priority)");
            // Mark voluntary release (same reasoning as RequestDisconnect above).
            m_voluntarilyReleased.store(true);
            log::handover("STATE   m_voluntarilyReleased = true (voluntary handover)");

            // W4 fix: record app + deferred pause retry (same as RequestDisconnect path).
            {
                std::scoped_lock lk{m_voluntaryReleaseAppMutex};
                m_voluntaryReleaseAppId = m_media.currentAppId();
            }
            m_lastVoluntaryReleaseAt.store(tpToNs(std::chrono::steady_clock::now()));
            std::thread([this]() {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                if (m_state.load() != OwnershipState::LocalPc) {
                    log::handover("ACTION  Deferred pause retry (2s) — suppressing app resume after release");
                    m_media.tryPauseActive();
                    m_media.tryPauseAllSessions();
                    m_media.tryPauseViaMediaKey();
                    m_media.tryPauseAllBrowserWindows();
                }
            }).detach();

            // Set the anti-pingpong timestamp first (see comment in ACCEPTED above).
            m_lastLostOwnership.store(tpToNs(std::chrono::steady_clock::now()));
            m_media.tryPauseActive();
            m_media.tryPauseAllSessions();
            m_media.tryPauseViaMediaKey();
            m_media.tryPauseAllBrowserWindows();
            m_airpods.disconnect();
            setState(OwnershipState::RemoteAndroid);
            if (m_peers.isAnyConnected()) {
                log::handover("OUT     kAirPodsDisconnected -> all peers");
                m_peers.sendPacket(kAirPodsDisconnected);
            }
            break;
        }

        case Incoming::AirPodsConnected: {
            // Peer announces it now has the AirPods. If we previously held them, pause
            // local media so anything still routing through PC speakers (Spotify after
            // AirPods left, browser audio, etc.) doesn't leak. Check state BEFORE the
            // transition so we don't pause on a peer's idempotent re-assertion when we
            // were already Remote.
            const bool wasLocalPc = (m_state.load() == OwnershipState::LocalPc);
            log::handover("IN      kAirPodsConnected from peer -> STATE RemoteAndroid");

            // Record both timestamps NOW, before any potentially-blocking work:
            //   - m_lastPeerOwnershipClaim: tells the BT falling-edge logic the peer
            //     just intentionally claimed, so we skip any reclaim attempt.
            //   - m_lastLostOwnership: anti-pingpong window for the media-start
            //     callback that fires when Chrome auto-resumes after the device
            //     change. Without this, a Pixel/Xiaomi handover triggers a counter-
            //     takeover ~50ms later when Chrome resumes on PC speakers.
            const auto nowNs = tpToNs(std::chrono::steady_clock::now());
            m_lastPeerOwnershipClaim.store(nowNs);
            m_lastLostOwnership.store(nowNs);

            if (wasLocalPc) {
                m_media.tryPauseActive();
                m_media.tryPauseAllSessions();
                m_media.tryPauseViaMediaKey();
                m_media.tryPauseAllBrowserWindows();
            }
            setState(OwnershipState::RemoteAndroid);
            break;
        }

        case Incoming::AirPodsDisconnected:
            log::handover("IN      kAirPodsDisconnected from peer (waiting for local trigger)");
            // Record the timestamp and wake any takeover thread that may be waiting
            // for the peer to confirm release of the AirPods. This lets the BT
            // connect attempt fire immediately on peer ack rather than after a
            // fixed 1.5 s timer, cutting best-case handover latency by ~1.4 s.
            m_lastPeerAckAt.store(tpToNs(std::chrono::steady_clock::now()));
            m_peerAckCv.notify_all();
            // Don't claim ownership just because remote dropped — wait for our own media event.
            break;

        case Incoming::RequestConnectionStatus: {
            log::debug("Peer requested connection status");
            const auto& reply = (m_state.load() == OwnershipState::LocalPc)
                                ? kAirPodsConnected
                                : kAirPodsDisconnected;
            m_peers.sendPacket(reply);
            break;
        }

        case Incoming::RequestBatteryBytes:
        case Incoming::RequestAncBytes:
            log::debug("Peer requested battery/ANC bytes — not supported in Windows v1");
            break;

        case Incoming::RelayHeader:
            log::debug("Peer relayed AACP packet — Windows v1 ignores AACP relay");
            break;

        case Incoming::WindowsAudioActive:
        case Incoming::WindowsAudioIdle:
            // These packets flow Windows -> Android only; ignore if received from Android.
            log::debug("Ignoring audio-state packet from Android (not expected)");
            break;

        case Incoming::Unknown:
            log::debug("Unknown packet ({} bytes)", data.size());
            break;
    }
}

void HandoverController::onPeerConnectionChanged(bool connected) {
    if (connected) {
        log::handover("PEER    Android connected via CrossDevice RFCOMM");
        // Re-sync cached state with reality before announcing.
        const bool airpodsHere = m_airpods.isClassicallyConnected();
        const bool wasLocalPc  = (m_state.load() == OwnershipState::LocalPc);

        // Gate the sync broadcast on setState's return value. If state didn't
        // change (e.g. already RemoteAndroid before this peer reconnected) don't
        // broadcast — the other already-connected peers already know. Without this
        // gate, every RFCOMM reconnect of any one peer floods the whole mesh with
        // kAirPodsDisconnected at the reconnect-backoff cadence (~2 s).
        const bool stateChanged = setState(airpodsHere ? OwnershipState::LocalPc
                                                       : OwnershipState::RemoteAndroid);
        if (stateChanged) {
            log::handover("OUT     {} -> all peers (sync on connect, state changed)",
                airpodsHere ? "kAirPodsConnected" : "kAirPodsDisconnected");
            m_peers.sendPacket(airpodsHere
                ? crossdevice::kAirPodsConnected
                : crossdevice::kAirPodsDisconnected);
        } else {
            // Newly-connected peer will learn our state from the next
            // RequestConnectionStatus keep-alive it sends within 20 s.
            log::handover("SKIP    State unchanged on peer connect ({}) — no broadcast",
                airpodsHere ? "LocalPc" : "RemoteAndroid");
        }

        // If we held ownership (LocalPc) but the AirPods are gone, the peer grabbed them
        // via Bluetooth before the RFCOMM coordination channel was established (bypassing
        // the protocol). Now that RFCOMM is up, reclaim them.
        if (wasLocalPc && !airpodsHere) {
            log::handover("RECLAIM Peer grabbed AirPods before RFCOMM was up — reclaiming now");
            onMediaPlayingChanged(true);
        }
    } else {
        log::handover("PEER    Android disconnected");
    }
}

void HandoverController::onCallActive() {
    // Teams/WASAPI-specific entry point. Suppressed after a voluntary release so an
    // ongoing Teams call that is now routing through the peer's AirPods doesn't
    // cause Windows to fight back. GSMTC-triggered media (YouTube etc.) goes through
    // onMediaPlayingChanged directly and is not gated here — GSMTC is edge-triggered
    // so a session that stays "playing" never re-fires anyway.
    if (m_voluntarilyReleased.load()) {
        log::handover("SKIP    Voluntarily released — suppressing call re-trigger until session ends");
        return;
    }
    onMediaPlayingChanged(true);
}

void HandoverController::onTeamsCallEnded() {
    // Teams session expired (call definitively ended). Clear the voluntary-release
    // gate so the next new call can trigger a claim.
    if (m_voluntarilyReleased.exchange(false))
        log::handover("STATE   m_voluntarilyReleased cleared (Teams session expired)");
}

}
