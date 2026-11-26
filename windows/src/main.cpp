#include <windows.h>
#include <shellapi.h>

#include <winrt/base.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

#include "AirPodsConnector.hpp"
#include "BleProximityWatcher.hpp"
#include "HandoverController.hpp"
#include "Logger.hpp"
#include "MediaPlaybackWatcher.hpp"
#include "PeerRegistry.hpp"
#include "SettingsStore.hpp"
#include "TeamsCallWatcher.hpp"
#include "BtHandoverSetup.hpp"
#include "PeerServer.hpp"
#include "v2/PeerV2Identity.hpp"
#include "v2/PeerV2Mesh.hpp"
#include "PlaybackSilencer.hpp"
#include "PlaybackHold.hpp"
#include "HoldOverlay.hpp"
#include "SessionMute.hpp"
#include "TraceUploader.hpp"
#include "TrayIcon.hpp"
#include "crossdevice_protocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <thread>
#include <memory>
#include <mutex>
#include <optional>

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
using namespace winrt::Windows::Devices::Enumeration;

namespace Propods {

namespace {

std::wstring stateLabel(OwnershipState s) {
    switch (s) {
        case OwnershipState::LocalPc:       return L"AirPods: on this PC";
        case OwnershipState::RemoteAndroid: return L"AirPods: on another device";
        default:                            return L"AirPods: unknown";
    }
}

struct DiscoveredPeer {
    std::uint64_t address{};
    std::wstring  name;
};

// Returns ALL paired devices that advertise `serviceUuid` (the shared
// coordination service: every EveryPods peer listens on it).
std::vector<DiscoveredPeer> discoverPeers(std::string_view serviceUuid) {
    std::vector<DiscoveredPeer> found;
    try {
        auto rfcommId = RfcommServiceId::FromUuid(uuidFromString(serviceUuid));

        // Fast path: collect everything the SDP cache already knows about.
        auto selector = RfcommDeviceService::GetDeviceSelector(rfcommId);
        auto cachedHits = DeviceInformation::FindAllAsync(selector).get();
        for (auto&& hit : cachedHits) {
            try {
                auto service = RfcommDeviceService::FromIdAsync(hit.Id()).get();
                if (!service || !service.Device()) continue;
                auto bdev = service.Device();
                log::info("Discovered peer (cached SDP): {} ({})",
                    to_string(bdev.Name()),
                    SettingsStore::formatBluetoothAddress(bdev.BluetoothAddress()));
                found.push_back({bdev.BluetoothAddress(), std::wstring(bdev.Name())});
            } catch (...) {}
        }

        // Slow path: probe paired devices not yet in the SDP cache.
        auto pairedSelector = BluetoothDevice::GetDeviceSelectorFromPairingState(true);
        auto paired = DeviceInformation::FindAllAsync(pairedSelector).get();
        log::debug("Paired classic-Bluetooth devices: {}", paired.Size());

        for (auto&& info : paired) {
            try {
                auto bdev = BluetoothDevice::FromIdAsync(info.Id()).get();
                if (!bdev) continue;
                auto addr = bdev.BluetoothAddress();
                // Skip addresses already collected from the cache.
                bool alreadyFound = false;
                for (auto& p : found) if (p.address == addr) { alreadyFound = true; break; }
                if (alreadyFound) continue;
                log::debug("  Probing {} ({})...",
                    to_string(bdev.Name()),
                    SettingsStore::formatBluetoothAddress(addr));
                auto services = bdev.GetRfcommServicesForIdAsync(rfcommId, BluetoothCacheMode::Uncached).get();
                if (services.Services().Size() > 0) {
                    log::info("Discovered peer (uncached SDP): {} ({})",
                        to_string(bdev.Name()),
                        SettingsStore::formatBluetoothAddress(addr));
                    found.push_back({addr, std::wstring(bdev.Name())});
                }
            } catch (const hresult_error& e) {
                log::debug("    skipped (probe failed: {})", to_string(e.message()));
            }
        }

        if (found.empty()) {
            log::warn("No paired device advertises the CrossDevice UUID. "
                      "Make sure: (1) the other device is paired with this PC, "
                      "(2) EveryPods is running on it with handover enabled.");
        }
    } catch (const hresult_error& e) {
        log::error("Peer discovery failed: 0x{:08X} {}",
            (std::uint32_t)e.code().value, to_string(e.message()));
    }
    return found;
}

std::optional<std::uint64_t> discoverAirPods() {
    try {
        // Match paired audio devices whose name contains "AirPods".
        auto selector = BluetoothDevice::GetDeviceSelectorFromPairingState(true);
        auto devices = DeviceInformation::FindAllAsync(selector).get();
        for (auto&& info : devices) {
            auto name = std::wstring(info.Name());
            if (name.find(L"AirPods") != std::wstring::npos) {
                auto bdev = BluetoothDevice::FromIdAsync(info.Id()).get();
                if (bdev) {
                    log::info("Found paired AirPods: {} ({})",
                        to_string(bdev.Name()),
                        SettingsStore::formatBluetoothAddress(bdev.BluetoothAddress()));
                    return bdev.BluetoothAddress();
                }
            }
        }
        log::warn("No paired AirPods found. Pair them in Windows Settings first.");
        return std::nullopt;
    } catch (const hresult_error& e) {
        log::error("AirPods discovery failed: {}", to_string(e.message()));
        return std::nullopt;
    }
}

}

}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    using namespace Propods;

    // Enforce single instance: if another Propods process is already running,
    // exit silently. The mutex is released automatically when this process exits.
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\PropodsSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    log::traceFile();  // open now so tracePath() is set (still single-threaded here)
    log::info("Durable trace: {}", log::toUtf8(log::tracePath().wstring()));

    SettingsStore store;
    Settings settings = store.load();

    if (settings.peerAddresses.empty()) {
        auto discovered = discoverPeers(crossdevice::kServiceUuid);
        if (!discovered.empty()) {
            for (auto& p : discovered) settings.peerAddresses.push_back(p.address);
            store.save(settings);
        }
    }
    if (!settings.airpodsAddress) {
        if (auto a = discoverAirPods()) {
            settings.airpodsAddress = a;
            store.save(settings);
        }
    }

    AirPodsConnector airpods{settings.airpodsAddress.value_or(0)};
    PeerRegistry peers;
    MediaPlaybackWatcher media;
    // Playback hold (off by default): holds this PC's audio while the AirPods
    // connect and drives the progress panel. Declared before the controller so
    // it outlives every thread that may still signal it.
    SessionMute holdMute;
    holdMute.restoreLeftovers();  // undo per-app mutes left by a crash or kill
    PlaybackHold hold{media, holdMute};
    hold.setEnabled(settings.holdPlayback);
    HandoverController controller{peers, airpods, media};
    TeamsCallWatcher teamsWatcher;
    // Apps that take or keep the AirPods, beyond the built-in ones (tray menu).
    teamsWatcher.triggerApps().setUserApps(settings.triggerApps);
    for (const auto& app : teamsWatcher.triggerApps().userApps())
        log::info("Trigger app: {} ({})", log::toUtf8(app.exe), triggerRulesText(app));
    teamsWatcher.seedSeenApps(settings.seenApps);
    BleProximityWatcher bleWatcher;
    TrayIcon tray;

    controller.setAutoRoute(settings.autoRoute);
    controller.setLidOpenConnect(settings.lidOpenConnect);
    controller.setLidLease(settings.lidLease);
    // A case lid opened near this PC: connect if this PC used the AirPods last.
    bleWatcher.setOnLidOpened([&controller]() { controller.onLidOpened(); });

    // BLE proximity gate: don't initiate a takeover when the AirPods are stowed
    // in their case (would otherwise hijack audio from a Jabra/Dell headset the
    // user is actively on). Fails open if BLE is unavailable.
    bleWatcher.start();

    // Peer protocol v2: publishes this PC's AirPods ownership to every peer and
    // answers their automatic-reconnect requests (see PeerV2Mesh.hpp). Audio use
    // comes from the existing media/Teams events, never from polling audio
    // sessions (polling made Windows drop idle AirPods in June).
    std::atomic<bool> mediaPlaying{false};
    std::atomic<bool> takeOnMediaStart{settings.takeOnMediaStart};
    std::atomic<bool> takeOnCall{settings.takeOnCall};
    v2::PeerV2Mesh peerV2([&]() {
        v2::LocalState st;
        const std::uint64_t addr = airpods.address();
        if (addr == 0) return st;  // headset unknown -> ownership unknown
        st.headsetId = v2::headsetIdFromAddress(addr);
        const bool held = airpods.isClassicallyConnectedWinRT();
        st.ownership = held ? v2::Ownership::Held : v2::Ownership::Released;
        // Only an open keep-app mic (Teams, a meeting app, or one set in the tray) is a call. An audio session starting (VLC,
        // a Teams chat ping) is not, and its "ended" event can be hours late:
        // counting it reported a call for as long as the PC held the AirPods,
        // and phones then refused to take them back (25 Nov).
        st.audioUse = !held ? v2::AudioUse::Idle
                    : controller.callMicOpen() ? v2::AudioUse::Call
                    : mediaPlaying.load() ? v2::AudioUse::Media
                    : v2::AudioUse::Idle;
        return st;
    });

    // Don't take the AirPods while a peer holds a v2 reservation on us: it is
    // in the middle of connecting them after a case-open.
    controller.setAvailabilityProbe([&bleWatcher, &peerV2]() {
        if (peerV2.reservationActive()) {
            log::handover("SKIP    takeover: a peer is mid-connect (v2 reservation active)");
            return false;
        }
        return bleWatcher.airpodsAvailable();
    });
    // The hold, pre-mute and panel need a peer that holds the AirPods AND fresh
    // BLE evidence a bud is worn: a phone still reports "held" with the AirPods
    // in their case while its Bluetooth profile stays up.
    // Lid-open connect stands aside when a peer holds the AirPods.
    controller.setPeerHoldsProbe([&peerV2]() { return peerV2.anyPeerHoldsAirPods(); });
    controller.setHoldProbe([&peerV2, &bleWatcher]() {
        const bool held = peerV2.anyPeerHoldsAirPods();
        const bool worn = bleWatcher.budLikelyWorn();
        if (!held || !worn)
            log::debug("Hold gate: peer holds AirPods={} bud worn={}", held, worn);
        return held && worn;
    });

    if (!tray.create(hInstance)) return 1;
    tray.setStatus(stateLabel(controller.state()));
    // Tray icon colour: green on this PC, blue while a handover to this PC
    // runs, grey on another device.
    auto ownershipIcon = [](OwnershipState s) {
        return s == OwnershipState::LocalPc ? TrayIcon::IconState::Here : TrayIcon::IconState::Away;
    };
    tray.setIconState(ownershipIcon(controller.state()));

    // Progress panel for handovers to this PC (never takes focus).
    HoldOverlay overlay{hInstance, hold};
    overlay.create();
    overlay.setOnEndSession([&holdMute]() { holdMute.restoreAll("Windows is signing out"); });
    hold.setOnStatusChange([&overlay, &tray, &controller, ownershipIcon](const std::optional<HoldStatus>& s) {
        overlay.update(s);
        const bool connecting = s && (s->phase == HoldPhase::RequestingRelease ||
                                      s->phase == HoldPhase::Connecting || s->phase == HoldPhase::Routing);
        tray.setIconState(connecting ? TrayIcon::IconState::Connecting : ownershipIcon(controller.state()));
    });
    hold.setHooks({
        /*deviceName=*/[&airpods]() { return airpods.displayName(); },
        /*airPodsAreDefaultOutput=*/[&airpods]() { return airpods.isDefaultRenderDevice(); },
        /*preMuteAllowed=*/[&controller, &takeOnMediaStart]() {
            return takeOnMediaStart.load() && controller.mediaTakeoverLikely();
        },
        /*retryTakeover=*/[&controller](HoldKind kind) { controller.retryTakeover(kind); },
        /*cancelTakeover=*/[&controller]() { controller.cancelTakeover(); },
    });
    controller.setPlaybackHold(&hold);
    media.setOnSessionPlayback([&hold](const std::wstring& appId, bool playing) {
        hold.onSessionPlaybackChanged(appId, playing);
    });
    teamsWatcher.setOnRenderSessionActive([&hold, &teamsWatcher](const AudioSessionRef& session) {
        // No pre-mute for an app that doesn't take the AirPods on play: no takeover follows.
        if (!teamsWatcher.triggerApps().resolve(session.exe).takeOnPlay) return;
        hold.onRenderSessionActive(session);
    });

    // When the AirPods leave this PC, stop PC audio so it doesn't continue out of
    // the speakers: pause every playing media session, then mute what is still
    // audible. Only our own mutes are undone, when the AirPods come back, when
    // media starts on the PC, or from the tray.
    PlaybackSilencer silencer;
    silencer.setEnabled(settings.pauseOnRelease);
    silencer.restoreLeftovers();
    silencer.setStillReleased([&controller]() { return controller.state() != OwnershipState::LocalPc; });
    // Tell the user in the same panel as the handover progress (not a toast).
    silencer.setOnDone([&overlay](const PlaybackSilencer::Result& r) {
        std::wstring apps;
        for (const auto& n : r.names) { if (!apps.empty()) apps += L", "; apps += n; }
        std::wstring text = r.muted > 0
            ? std::format(L"Paused {}, muted {}: {}", r.paused, r.muted, apps)
            : std::format(L"Paused: {}", apps);
        overlay.showNotice(L"AirPods moved to another device", text, /*offerUnmute=*/r.muted > 0);
    });
    // The notice's Unmute button (UI thread, like the tray's Unmute item).
    overlay.setOnUnmute([&silencer]() { silencer.restoreMutes("panel"); });
    auto restoreAsync = [&silencer](const char* reason) {
        if (!silencer.hasMuted()) return;
        std::thread([&silencer, reason]() {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            silencer.restoreMutes(reason);
        }).detach();
    };

    std::atomic<OwnershipState> lastOwnership{OwnershipState::Unknown};
    // When the AirPods leave, Windows moves output to the speakers and apps'
    // audio sessions reappear there as "active". That is not the user asking
    // for the AirPods back (on 2026-11-21 VLC did exactly this and pulled them
    // back from the Pixel within 5 s), so audio-session triggers are ignored
    // for a short while after losing ownership.
    std::atomic<std::int64_t> lostOwnershipAtMs{0};
    controller.setOnStateChanged([&tray, &hold, ownershipIcon, &bleWatcher, &silencer, &lastOwnership, &lostOwnershipAtMs, restoreAsync](OwnershipState s) {
        tray.setStatus(stateLabel(s));
        // While a handover runs the icon stays "connecting"; the hold sets it after.
        if (!hold.isActive()) tray.setIconState(ownershipIcon(s));
        bleWatcher.setPaused(s == OwnershipState::LocalPc);
        const OwnershipState previous = lastOwnership.exchange(s);
        if (previous == OwnershipState::LocalPc && s != OwnershipState::LocalPc) {
            lostOwnershipAtMs.store(static_cast<std::int64_t>(GetTickCount64()));
            silencer.silenceAsync("AirPods left this PC");
        }
        if (s == OwnershipState::LocalPc)
            restoreAsync("AirPods back on this PC");
    });

    std::mutex settingsMutex;
    // An app first seen in a call got all three settings: save them.
    teamsWatcher.setOnCallAppAdopted([&](const std::wstring&) {
        std::scoped_lock lk{settingsMutex};
        settings.triggerApps = teamsWatcher.triggerApps().userApps();
        store.save(settings);
    });
    // Remember the apps that used sound or the mic (the 40 most recent), so the
    // tray lists them after a restart. Saved when the menu opens and on quit.
    auto persistSeenApps = [&]() {
        std::vector<std::wstring> recent;
        for (const auto& seen : teamsWatcher.seenAudioApps()) {
            if (recent.size() >= 40) break;
            recent.push_back(seen.exe);
        }
        std::scoped_lock lk{settingsMutex};
        if (recent == settings.seenApps) return;
        settings.seenApps = std::move(recent);
        store.save(settings);
    };

    // Provide live status to the tray menu (queried each time the menu opens).
    // Apps as the "take or keep" menu last showed them: its commands carry an
    // index into this list. Both run on the tray thread.
    std::vector<std::wstring> menuApps;
    tray.setStatusProvider([&]() -> TrayIcon::StatusInfo {
        TrayIcon::StatusInfo info;
        info.airpodsConnected = airpods.isClassicallyConnected();
        info.autoRouteEnabled = controller.autoRoute();
        info.pauseOnRelease = silencer.enabled();
        info.takeOnMediaStart = takeOnMediaStart.load();
        info.takeOnCall = takeOnCall.load();
        info.holdPlayback = hold.enabled();
        info.lidOpenConnect = controller.lidOpenConnect();
        info.hasMutedApps = silencer.hasMuted();
        for (auto& p : peers.peerInfos())
            info.peers.push_back({p.address, p.name, p.connected});
        persistSeenApps();
        // One list: built-ins, the apps with saved settings and every remembered app, by name.
        std::vector<TrayIcon::TriggerAppStatus> apps;
        auto& triggerList = teamsWatcher.triggerApps();
        for (const auto& app : triggerList.listed())
            apps.push_back({app.exe, app.takeOnMic, app.takeOnPlay, app.keepOnMic, app.builtIn, app.changed});
        for (const auto& seen : teamsWatcher.seenAudioApps()) {
            if (std::any_of(apps.begin(), apps.end(), [&](const auto& a) { return a.exe == seen.exe; })) continue;
            const TriggerApp app = triggerList.resolve(seen.exe);
            apps.push_back({app.exe, app.takeOnMic, app.takeOnPlay, app.keepOnMic, app.builtIn, app.changed});
        }
        std::sort(apps.begin(), apps.end(), [](const auto& a, const auto& b) { return a.exe < b.exe; });
        if ((int)apps.size() > TrayIcon::kMaxMenuApps) apps.resize(TrayIcon::kMaxMenuApps);
        menuApps.clear();
        for (const auto& app : apps) menuApps.push_back(app.exe);
        info.apps = std::move(apps);
        return info;
    });

    // Answer a peer's "add BT handover device" exchange. Without this the peer
    // keeps the PC pending and refuses its CrossDevice connection.
    // When a peer enables handover with this PC, start coordinating with it.
    // Persist the lid-open lease ("this PC used the AirPods last") across restarts.
    controller.setOnLidLeaseChanged([&](bool held) {
        std::scoped_lock lk{settingsMutex};
        if (settings.lidLease == held) return;
        settings.lidLease = held;
        store.save(settings);
    });
    {   // It may have changed before the callback was set (e.g. AirPods already here).
        std::scoped_lock lk{settingsMutex};
        if (settings.lidLease != controller.lidLease()) {
            settings.lidLease = controller.lidLease();
            store.save(settings);
        }
    }
    BtHandoverSetupServer handoverSetup;
    handoverSetup.setOnChange([&](std::uint64_t peer, bool enabled) {
        if (!enabled) {
            log::info("Peer {} removed BT handover with this PC (peer kept; remove it from the tray if wanted)",
                SettingsStore::formatBluetoothAddress(peer));
            return;
        }
        std::scoped_lock lk{settingsMutex};
        auto& addrs = settings.peerAddresses;
        if (std::find(addrs.begin(), addrs.end(), peer) != addrs.end()) return;
        addrs.push_back(peer);
        store.save(settings);
        peers.addPeer(peer);
        peerV2.addPeer(peer);
        log::info("Added peer {} after it enabled BT handover",
            SettingsStore::formatBluetoothAddress(peer));
    });
    handoverSetup.start();
    // An Android peer accepts our links only after it has finished adding this
    // PC; current versions wait for the PC to start that exchange. A peer that
    // closes our link at once is asked to finish it (at most every 2 min).
    peers.setOnPeerRejected([&handoverSetup](std::uint64_t address) { handoverSetup.authorize(address); });

    // Any paired peer may dial this PC. One that is not a peer yet becomes one
    // (Bluetooth pairing is the trust boundary): save it and add it to the v2 mesh.
    peers.setOnPeerAdded([&](std::uint64_t address) {
        {
            std::scoped_lock lk{settingsMutex};
            auto& addrs = settings.peerAddresses;
            if (std::find(addrs.begin(), addrs.end(), address) != addrs.end()) return;
            addrs.push_back(address);
            store.save(settings);
        }
        peerV2.addPeer(address);
        log::info("Saved peer {} (it linked to this PC)",
            SettingsStore::formatBluetoothAddress(address));
    });
    PeerServer peerServer;
    peerServer.setOnConnection([&peers](std::uint64_t address,
                                        winrt::Windows::Networking::Sockets::StreamSocket socket) {
        peers.acceptInbound(address, socket);
    });

    peers.setOnPacket([&controller](std::span<const std::uint8_t> data) {
        controller.onIncomingPacket(data);
    });
    peers.setOnState([&controller](bool connected) {
        controller.onPeerConnectionChanged(connected);
    });

    media.setCallback([&controller, &mediaPlaying, &takeOnMediaStart, &peerV2, &media, &teamsWatcher, restoreAsync](bool playing) {
        mediaPlaying.store(playing);
        if (playing) restoreAsync("media started on this PC");
        // An app without "Take the AirPods when it starts playing" still counts
        // as playing, but its start raises no takeover.
        if (playing) {
            const std::wstring appId = media.lastStartedAppId();
            if (!teamsWatcher.triggerApps().takesOnMediaStart(appId)) {
                log::handover("SKIP    media start by '{}': its app doesn't take the AirPods when it starts playing",
                              log::toUtf8(appId));
                peerV2.refreshLocalState();
                return;
            }
        }
        // "Take AirPods when this PC starts playing media" off: never raise a
        // media-start takeover (stop notifications still pass through).
        if (!playing || takeOnMediaStart.load()) controller.onMediaPlayingChanged(playing);
        peerV2.refreshLocalState();
    });

    // WASAPI-based trigger (Teams calls, VLC, the user's trigger apps) feeds the handover path via
    // onCallActive(), which applies the voluntary-release gate in addition to
    // the standard debounce and anti-pingpong guards. This prevents an ongoing
    // Teams call from re-triggering a claim after Windows voluntarily released
    // AirPods to a peer.
    teamsWatcher.setOnTriggered([&controller, &takeOnCall, &takeOnMediaStart, &peerV2, &silencer, &lostOwnershipAtMs](bool comms) {
        const std::int64_t sinceLost = static_cast<std::int64_t>(GetTickCount64()) - lostOwnershipAtMs.load();
        if (lostOwnershipAtMs.load() != 0 && sinceLost < 15'000) {
            log::handover("SKIP    audio-session trigger {} ms after the AirPods left (output moved to speakers)", sinceLost);
            return;
        }
        // An app we muted for the phone keeps its audio session active; that is
        // not the user asking for the AirPods back (seen with VLC: it pulled them
        // back from the Pixel a minute later). Real media starts still trigger.
        if (silencer.hasMuted()) {
            log::handover("SKIP    audio-session trigger: apps are muted for the phone");
            return;
        }
        // Call apps follow "take on call", media apps (VLC, user media apps)
        // "take on media start".
        if (comms ? takeOnCall.load() : takeOnMediaStart.load()) controller.onCallActive(/*isCall=*/comms);
        peerV2.refreshLocalState();
    });

    // A keep app's mic open (Teams, a meeting app, or one set in the tray) = in a call or
    // meeting: protects the AirPods from a phone
    // call takeover while they are on this PC (kPeerAudioActive to peers).
    controller.setOnCallMicSettled([&peerV2]() { peerV2.refreshLocalState(); });
    teamsWatcher.setOnCallMicChanged([&controller](bool open) { controller.onCallMicChanged(open); });

    // When the Teams audio session expires (call ended), clear the voluntary-release
    // gate so the next new call can trigger a fresh claim.
    teamsWatcher.setOnSessionEnded([&controller, &peerV2]() {
        controller.onTeamsCallEnded();
        peerV2.refreshLocalState();
    });

    tray.setMenuHandler([&](int cmd) {
        switch (cmd) {
            case TrayIcon::kCmdQuit:
                tray.postQuit();
                break;
            case TrayIcon::kCmdPairAndroid: {
                auto discovered = discoverPeers(crossdevice::kServiceUuid);

                // Filter to devices not yet configured.
                std::vector<DiscoveredPeer> newPeers;
                {
                    std::scoped_lock lk{settingsMutex};
                    for (auto& p : discovered) {
                        const auto& known = settings.peerAddresses;
                        if (std::find(known.begin(), known.end(), p.address) == known.end())
                            newPeers.push_back(p);
                        else
                            log::info("Peer {} already registered",
                                SettingsStore::formatBluetoothAddress(p.address));
                    }
                }

                if (newPeers.empty()) {
                    if (discovered.empty()) {
                        MessageBoxW(nullptr,
                            L"No paired device running EveryPods found.\n\n"
                            L"Make sure:\n"
                            L"  \x2022 The device is paired in Windows Bluetooth settings\n"
                            L"  \x2022 EveryPods is running on it with handover enabled",
                            L"Propods \x2014 Add peer", MB_OK | MB_ICONINFORMATION);
                    } else {
                        MessageBoxW(nullptr,
                            L"All discovered devices are already added as peers.",
                            L"Propods \x2014 Add peer", MB_OK | MB_ICONINFORMATION);
                    }
                    break;
                }

                // Ask the user to confirm each new device.
                for (auto& p : newPeers) {
                    auto addrStr = SettingsStore::formatBluetoothAddress(p.address);
                    std::wstring msg = std::wstring(L"Add this device?")
                        + L"\n\n" + p.name + L"\n" + std::wstring(addrStr.begin(), addrStr.end());
                    if (MessageBoxW(nullptr, msg.c_str(), L"Propods \x2014 Add peer",
                                    MB_YESNO | MB_ICONQUESTION) != IDYES) continue;
                    {
                        std::scoped_lock lk{settingsMutex};
                        settings.peerAddresses.push_back(p.address);
                        store.save(settings);
                    }
                    peers.addPeer(p.address);
                    peerV2.addPeer(p.address);
                    log::info("Added new peer: {} ({})", to_string(p.name), addrStr);
                }
                break;
            }
            case TrayIcon::kCmdPairAirPods:
                if (auto a = discoverAirPods()) {
                    settings.airpodsAddress = a;
                    store.save(settings);
                    airpods.setAddress(*a);
                }
                break;
            case TrayIcon::kCmdOpenTraces: {
                const auto dir = log::traceDir();
                ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
            case TrayIcon::kCmdSendTraces: {
                // Bluetooth transfer of a few MB takes seconds to minutes; keep it off
                // the UI thread and refuse overlapping sends.
                static std::atomic<bool> sending{false};
                if (sending.exchange(true)) break;
                tray.notify(L"Sending traces to peers",
                            L"Only new or changed trace files are sent. You'll get a summary when done.");
                // Every peer; one that does not offer trace upload just reports it.
                auto infos = peers.peerInfos();
                std::thread([&tray, infos]() {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    std::wstring summary;
                    for (const auto& info : infos) {
                        const auto r = uploadTraces(info.address, std::chrono::hours{72});
                        summary += info.name + L": ";
                        summary += r.error.empty()
                            ? std::format(L"{} sent, {} unchanged, {} rejected", r.stored, r.skipped, r.rejected)
                            : L"failed: " + std::wstring(winrt::to_hstring(r.error));
                        summary += L"\n";
                    }
                    if (infos.empty()) summary = L"No peers configured.";
                    tray.notify(L"Trace upload finished", summary);
                    sending.store(false);
                }).detach();
                break;
            }
            case TrayIcon::kCmdConnectAirPods:
                // Blocking operation — run on a detached thread so the UI stays responsive.
                std::thread([&airpods]() {
                    log::info("Manual connect: initiating BT connection to AirPods...");
                    if (airpods.connect()) {
                        airpods.setAsDefaultAudioDevice();
                        log::info("Manual connect: done — AirPods set as default device");
                    } else {
                        log::warn("Manual connect: BT connect failed");
                    }
                }).detach();
                break;
            case TrayIcon::kCmdConnectMic:
                // Same as Sound settings -> Recording -> right-click -> Connect.
                std::thread([&airpods]() {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    log::handover("MIC     tray: connecting the AirPods mic");
                    if (!airpods.reconnectHandsFree())
                        log::handover("MIC     tray: no Bluetooth audio driver accepted the mic connect");
                }).detach();
                break;
            case TrayIcon::kCmdUseAirPodsAudio:
                // The routing pass Propods runs on connect: default output and
                // input for every role, and Teams' devices.
                std::thread([&airpods]() {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    log::handover("ROUTE   tray: use AirPods for sound and mic");
                    airpods.setAsDefaultAudioDevice();
                }).detach();
                break;
            case TrayIcon::kCmdTogglePause: {
                const bool nowEnabled = !silencer.enabled();
                silencer.setEnabled(nowEnabled);
                settings.pauseOnRelease = nowEnabled;
                store.save(settings);
                log::info("Pause PC audio when AirPods leave: {}", nowEnabled ? "ON" : "OFF");
                break;
            }
            case TrayIcon::kCmdToggleTakeOnMedia: {
                const bool nowEnabled = !takeOnMediaStart.load();
                takeOnMediaStart.store(nowEnabled);
                settings.takeOnMediaStart = nowEnabled;
                store.save(settings);
                log::info("Take AirPods when this PC starts playing media: {}", nowEnabled ? "ON" : "OFF");
                break;
            }
            case TrayIcon::kCmdToggleTakeOnCall: {
                const bool nowEnabled = !takeOnCall.load();
                takeOnCall.store(nowEnabled);
                settings.takeOnCall = nowEnabled;
                store.save(settings);
                log::info("Take AirPods when a call starts on this PC: {}", nowEnabled ? "ON" : "OFF");
                break;
            }
            case TrayIcon::kCmdToggleLidOpen: {
                const bool nowEnabled = !controller.lidOpenConnect();
                controller.setLidOpenConnect(nowEnabled);
                {
                    std::scoped_lock lk{settingsMutex};
                    settings.lidOpenConnect = nowEnabled;
                    store.save(settings);
                }
                log::info("Connect AirPods when the case opens (last device): {}", nowEnabled ? "ON" : "OFF");
                break;
            }
            case TrayIcon::kCmdToggleHold: {
                const bool nowEnabled = !hold.enabled();
                hold.setEnabled(nowEnabled);
                settings.holdPlayback = nowEnabled;
                store.save(settings);
                log::info("Hold PC audio while AirPods connect: {}", nowEnabled ? "ON" : "OFF");
                break;
            }
            case TrayIcon::kCmdUnmuteApps: {
                const int n = silencer.restoreMutes("tray");
                tray.notify(L"PC audio", std::format(L"Unmuted {} app(s).", n));
                break;
            }
            case TrayIcon::kCmdToggleAutoRoute: {
                bool nowEnabled = !controller.autoRoute();
                controller.setAutoRoute(nowEnabled);
                settings.autoRoute = nowEnabled;
                store.save(settings);
                log::info("Auto-route audio: {}", nowEnabled ? "ON" : "OFF");
                break;
            }
            case TrayIcon::kCmdAutoStart:
                tray.toggleAutoStart();
                break;
            default:
                if (TrayIcon::isAppChoice(cmd)) {
                    const int idx = (cmd - TrayIcon::kCmdAppRule) / TrayIcon::kAppChoices;
                    const int choice = (cmd - TrayIcon::kCmdAppRule) % TrayIcon::kAppChoices;
                    if (idx >= (int)menuApps.size()) break;
                    const std::wstring exe = menuApps[idx];
                    auto& list = teamsWatcher.triggerApps();
                    bool changed = false;
                    if (choice == 3) {
                        changed = list.resetToBuiltIn(exe);
                    } else {
                        const auto rule = static_cast<TriggerRule>(choice);
                        changed = list.setRule(exe, rule, !list.resolve(exe).get(rule));
                    }
                    if (!changed) break;
                    log::info("Trigger apps: {} now {}", log::toUtf8(exe), triggerRulesText(list.resolve(exe)));
                    std::scoped_lock lk{settingsMutex};
                    settings.triggerApps = list.userApps();
                    store.save(settings);
                    break;
                }
                if (cmd >= TrayIcon::kCmdRemovePeer) {
                    // The index is into the peer list the menu was built from.
                    const int idx = cmd - TrayIcon::kCmdRemovePeer;
                    const auto infos = peers.peerInfos();
                    if (idx >= 0 && idx < (int)infos.size()) {
                        const std::uint64_t addr = infos[idx].address;
                        peers.removePeer(addr);
                        peerV2.removePeer(addr);
                        {
                            std::scoped_lock lk{settingsMutex};
                            std::erase(settings.peerAddresses, addr);
                            store.save(settings);
                        }
                        log::info("Removed peer: {}", SettingsStore::formatBluetoothAddress(addr));
                    }
                }
                break;
        }
    });

    for (auto addr : settings.peerAddresses)
        peers.addPeer(addr);

    // The v2 mesh starts first so a peer that dials right away (and is saved as
    // a new peer) can be added to it.
    peerV2.start(settings.peerAddresses);
    // Always started: any peer may dial this PC at any time.
    peers.start();
    // A peer's Bluetooth link coming up wakes the legacy dial loop (in
    // PeerRegistry) and the v2 one here, so a restarted peer relinks at once.
    peers.setOnPeerLinkUp([&peerV2](std::uint64_t address) { peerV2.retryNow(address); });
    peerServer.start();
    if (settings.peerAddresses.empty())
        log::warn("No peers configured. Use the tray menu to add a peer once it is paired in Windows.");

    media.start();
    teamsWatcher.start();

    tray.runMessageLoop();
    persistSeenApps();

    // Undo any hold mute now (media a hold paused stays paused); the hold
    // object itself lives on until the controller and its threads are gone.
    hold.shutdown();
    overlay.destroy();

    peers.setOnPeerLinkUp({});
    peers.setOnPeerRejected({});  // handoverSetup is destroyed before peers
    peerV2.stop();

    teamsWatcher.stop();
    bleWatcher.stop();
    media.stop();
    peerServer.stop();
    peers.stop();
    return 0;
}
