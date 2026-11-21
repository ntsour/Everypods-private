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

        // Initialised to actual state so the first poll doesn't produce a synthetic edge.
        bool lastBtConnected = m_airpods.isClassicallyConnectedWinRT();
        while (m_watcherRunning.load()) {
            try {
                // Resolve Unknown state if peers are now connected.
                // Guard the packet send on setState's return value: if onPeerConnectionChanged
                // raced us here and already resolved Unknown, setState returns false and we
                // skip the duplicate send.
                if (m_state.load() == OwnershipState::Unknown && m_peers.isAnyConnected()) {
                    bool airpodsHere = m_airpods.isClassicallyConnectedWinRT();
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
                // before RFCOMM is up). Runs every 1s poll — fast enough for the handover
                // feel while cheap enough not to stress the BT stack.
                bool btConnected = m_airpods.isClassicallyConnectedWinRT();
                if (btConnected && !lastBtConnected) {
                    // Rising edge: AirPods just appeared on Windows (X1, X3, C2 scenarios).
                    if (setState(OwnershipState::LocalPc)) {
                        log::handover("BT      AirPods connected (BT rising edge) — claiming ownership");
                        if (m_peers.isAnyConnected()) {
                            log::handover("OUT     kAirPodsConnected -> all peers (BT edge)");
                            m_peers.sendPacket(crossdevice::kAirPodsConnected);
                        }
                        // Route audio: one SetDefaultEndpoint pass (ACTIVE or UNPLUGGED fallback).
                        if (m_autoRoute.load()) m_airpods.setAsDefaultAudioDevice();
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
                        // So: always release ownership on BT falling edge while we held LocalPc.
                        log::handover("BT      AirPods disconnected (BT falling edge) — releasing");

                        // Set the anti-pingpong timestamp FIRST, before any potentially-blocking
                        // operation. Mirrors the fix in onIncomingPacket's release handlers. The
                        // pause calls below can take 10-50 ms each (GSMTC, SendInput), and the
                        // PlaybackInfoChanged callback runs on a separate GSMTC thread — without
                        // this, Chrome auto-resuming when audio falls back to PC speakers can
                        // fire onMediaPlayingChanged(true) before m_lastLostOwnership is set,
                        // and anti-pingpong fails to suppress the counter-takeover.
                        m_lastLostOwnership.store(tpToNs(std::chrono::steady_clock::now()));

                        // Do NOT pause the user's media. Propods must never touch playback
                        // (let Windows/the user manage it). Audio may briefly continue on PC
                        // speakers after the AirPods leave — acceptable, and far better than
                        // freezing the user's video as the pause sequence did.
                        if (setState(OwnershipState::RemoteAndroid)) {
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
            } catch (const std::exception& e) {
                log::warn("Audio watcher: exception during poll: {}", e.what());
                // Continue polling, don't crash
            }

            // Sliced sleep so shutdown is responsive (100 ms × 10 = 1000 ms poll interval).
            // Reduced from 500 ms to 1000 ms to cut legacy-BT and COM audio enumeration
            // overhead in half. BT-edge precision of 1 s is still fast enough for
            // handover responsiveness (the protocol handshake takes 1-3 s anyway).
            for (int i = 0; i < 10 && m_watcherRunning.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    // PURE TEST-REPLICA: do NOT pause the user's media. Pausing here (then resuming after
    // claiming the AirPods) is what froze YouTube when connect() was slow or the resume
    // misfired. We just wake the link and let Windows route the already-playing audio onto
    // the AirPods once it activates them — like selecting the device in the sound flyout.

    // Send kRequestDisconnect to peers immediately (no waiting) so the phone knows to
    // release — especially important for limited-mode devices (Xiaomi) that can't force-drop
    // A2DP but need the signal to stop playing and allow their link to drop naturally.
    // Then connect right away in parallel (don't wait for ack/grace). The phone releases
    // while we're already connecting; the AirPods switch their A2DP to Windows once free.
    const bool peerConnected = m_peers.isAnyConnected();
    if (peerConnected) {
        log::handover("OUT     kRequestDisconnect -> all peers");
        m_peers.sendPacket(crossdevice::kRequestDisconnect);
    }

    // Dispatch the blocking BT connect to a detached thread (the WinRT media-callback thread
    // must not block or the runtime terminates it).
    std::thread([this, peerConnected]() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        // Connect immediately while the phone is releasing. A phone cannot drop the
        // AirPods itself (no BLUETOOTH_PRIVILEGED); the AirPods decide when to accept
        // us, and on 2026-11-21 they ignored two attempts from a Xiaomi that had
        // already let go. Each attempt already waits a full page (~6 s) for the link,
        // so retry back to back: pauses between attempts only added 4.5 s, and three
        // attempts could give up on a slow switch.
        constexpr int kAttempts = 5;
        bool connected = false;
        for (int attempt = 1; attempt <= kAttempts && !connected; ++attempt) {
            log::handover("ACTION  BT connect attempt {}/{}{}", attempt, kAttempts,
                          attempt == 1 ? " (connect while phone releases in parallel)" : "");
            connected = m_airpods.connect();
            if (!connected && attempt < kAttempts)
                log::handover("RETRY   BT connect attempt {} failed — retrying now", attempt);
        }
        if (connected) {
            m_takeoverInFlight.store(false);
            // W2 fix: gate the ownership broadcast on a real state transition. The
            // 500 ms BT rising-edge watcher may have already observed the classical
            // link come up and claimed LocalPc (broadcasting kAirPodsConnected) a
            // moment before connect() returned here. Broadcasting unconditionally
            // then floods every peer with a duplicate kAirPodsConnected ~3-4 s after
            // the first. setState() returns true only for the thread that actually
            // performed the RemoteAndroid->LocalPc transition, so exactly one of the
            // two paths broadcasts.
            const bool claimed = setState(OwnershipState::LocalPc);
            m_lastLocalTakeover.store(tpToNs(std::chrono::steady_clock::now()));
            if (claimed && m_peers.isAnyConnected()) {
                log::handover("OUT     kAirPodsConnected -> all peers");
                m_peers.sendPacket(crossdevice::kAirPodsConnected);
            }
            // Route audio to AirPods: one synchronous SetDefaultEndpoint pass. If the
            // endpoint is already ACTIVE, this routes immediately. If still UNPLUGGED
            // (A2DP stream not yet open), the UNPLUGGED fallback in tryApplyAirPodsRouting
            // forces Windows to open the A2DP channel. No notifier, no 3s re-assert.
            if (m_autoRoute.load()) m_airpods.setAsDefaultAudioDevice();
        } else {
            log::handover("FAIL    BT connect failed after {} attempts — AirPods not acquired", kAttempts);
            // Anti-pingpong window so a GSMTC re-trigger doesn't immediately re-attempt.
            m_lastLostOwnership.store(tpToNs(std::chrono::steady_clock::now()));
            m_takeoverInFlight.store(false);
            // PURE TEST-REPLICA: nothing else. We never paused the user's media (nothing to
            // resume) and we do no routing. If the AirPods come up later, the BT rising-edge
            // watcher claims ownership and Windows routes to them natively.
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
            // Call-hold veto: do not yield AirPods when we are currently in an active
            // call session (Teams / Zoom / WebEx / etc.). All devices receive the same
            // VoIP ring simultaneously and each sends kRequestDisconnect — without this
            // veto the first requester blindly wins even when Windows is the one the
            // user is actually answering on. Music-only requests are unaffected (the
            // hasActiveCallSessions() check returns false for music-only audio). The
            // "requester always wins" rule still applies for non-call scenarios.
            if (m_airpods.hasActiveCallSessions()) {
                log::handover("IN      kRequestDisconnect — REJECTED (call-hold veto: active call/meeting session)");
                if (m_peers.isAnyConnected())
                    m_peers.sendPacket(crossdevice::kAirPodsConnected);   // re-assert to caller
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

            // W4 bookkeeping: record which app was playing so we can suppress its
            // re-trigger for the voluntary-release window (coordination, not playback).
            {
                std::scoped_lock lk{m_voluntaryReleaseAppMutex};
                m_voluntaryReleaseAppId = m_media.currentAppId();
            }
            m_lastVoluntaryReleaseAt.store(tpToNs(std::chrono::steady_clock::now()));

            // Record "lost ownership" NOW, before any potentially-blocking operation, so the
            // anti-pingpong window covers the whole release sequence (a media-start callback
            // firing during the disconnect must see a fresh timestamp).
            m_lastLostOwnership.store(tpToNs(std::chrono::steady_clock::now()));

            // Do NOT pause the user's media. Propods never touches playback — we just release
            // ownership and let the peer take the AirPods; Windows audio keeps playing on the
            // PC speakers (acceptable) rather than freezing the user's video.
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

            // W4 bookkeeping: record which app was playing (coordination, not playback).
            {
                std::scoped_lock lk{m_voluntaryReleaseAppMutex};
                m_voluntaryReleaseAppId = m_media.currentAppId();
            }
            m_lastVoluntaryReleaseAt.store(tpToNs(std::chrono::steady_clock::now()));

            // Set the anti-pingpong timestamp first (see comment in ACCEPTED above).
            m_lastLostOwnership.store(tpToNs(std::chrono::steady_clock::now()));
            // Do NOT pause the user's media — Propods never touches playback.
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

            // Do NOT pause the user's media — Propods never touches playback. The peer
            // now holds the AirPods; Windows audio (if any) keeps playing on the PC
            // speakers rather than freezing.
            (void)wasLocalPc;
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
        log::handover("PEER    peer connected (phone or computer)");
        // Re-sync cached state with reality before announcing.
        const bool airpodsHere = m_airpods.isClassicallyConnectedWinRT();
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

        // A takeover started while this peer was not connected sent its
        // kRequestDisconnect only to the peers connected then. If this peer
        // holds the AirPods it never heard it, and every connect attempt
        // fails (2026-11-21: Propods restarted, the Xiaomi reconnected 14 s
        // into the takeover, still holding). Repeat the request now; peers
        // that do not hold the AirPods just confirm kAirPodsDisconnected.
        if (m_takeoverInFlight.load() && !airpodsHere) {
            log::handover("OUT     kRequestDisconnect -> all peers (peer connected during takeover)");
            m_peers.sendPacket(crossdevice::kRequestDisconnect);
        }
    } else {
        log::handover("PEER    peer disconnected");
    }
}

void HandoverController::onCallActive() {
    // A Teams/WASAPI trigger is always an explicit user action (answered call,
    // joined meeting), never GSMTC auto-resume. Clear the voluntary-release
    // gate immediately — the user chose to take a call, so AirPods must move.
    // The 30s suppression window in onMediaPlayingChanged guards the GSMTC
    // path (Chrome auto-resume after device drop).
    if (m_voluntarilyReleased.exchange(false))
        log::handover("STATE   m_voluntarilyReleased cleared (call active)");
    onMediaPlayingChanged(true);
}

void HandoverController::onTeamsCallEnded() {
    // Teams session expired (call definitively ended). Clear the voluntary-release
    // gate so the next new call can trigger a claim.
    if (m_voluntarilyReleased.exchange(false))
        log::handover("STATE   m_voluntarilyReleased cleared (Teams session expired)");
}

}
