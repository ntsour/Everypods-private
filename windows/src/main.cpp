#include <windows.h>

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
#include "TrayIcon.hpp"
#include "crossdevice_protocol.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
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
        case OwnershipState::RemoteAndroid: return L"AirPods: on Android";
        default:                            return L"AirPods: unknown";
    }
}

struct DiscoveredPeer {
    std::uint64_t address{};
    std::wstring  name;
};

// Returns ALL paired devices that advertise the CrossDevice RFCOMM UUID.
std::vector<DiscoveredPeer> discoverAndroidPeers() {
    std::vector<DiscoveredPeer> found;
    try {
        winrt::guid rfcommGuid {
            0x1abbb9a4u, 0x10e4u, 0x4000u,
            { 0xa7, 0x5c, 0x89, 0x53, 0xc5, 0x47, 0x13, 0x42 }
        };
        auto rfcommId = RfcommServiceId::FromUuid(rfcommGuid);

        // Fast path: collect everything the SDP cache already knows about.
        auto selector = RfcommDeviceService::GetDeviceSelector(rfcommId);
        auto cachedHits = DeviceInformation::FindAllAsync(selector).get();
        for (auto&& hit : cachedHits) {
            try {
                auto service = RfcommDeviceService::FromIdAsync(hit.Id()).get();
                if (!service || !service.Device()) continue;
                auto bdev = service.Device();
                log::info("Discovered Android peer (cached SDP): {} ({})",
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
                    log::info("Discovered Android peer (uncached SDP): {} ({})",
                        to_string(bdev.Name()),
                        SettingsStore::formatBluetoothAddress(addr));
                    found.push_back({addr, std::wstring(bdev.Name())});
                }
            } catch (const hresult_error& e) {
                log::debug("    skipped (probe failed: {})", to_string(e.message()));
            }
        }

        if (found.empty()) {
            log::warn("No paired device advertises CrossDevice UUID. "
                      "Make sure: (1) the phone is paired with this PC, "
                      "(2) Handover is ON in the Propods Android app.");
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

    // Safety net: re-enable any audio endpoint a previous run disabled during a
    // competitor eviction but never restored (e.g. crash mid-eviction). Must run
    // before normal operation so a managed device is never left dead across restarts.
    AirPodsConnector::restoreDisabledEndpointsOnStartup();

    SettingsStore store;
    Settings settings = store.load();

    if (settings.androidAddresses.empty()) {
        auto discovered = discoverAndroidPeers();
        if (!discovered.empty()) {
            for (auto& p : discovered)
                settings.androidAddresses.push_back(p.address);
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
    HandoverController controller{peers, airpods, media};
    TeamsCallWatcher teamsWatcher;
    BleProximityWatcher bleWatcher;
    TrayIcon tray;

    controller.setAutoRoute(settings.autoRoute);

    // BLE proximity gate: don't initiate a takeover when the AirPods are stowed
    // in their case (would otherwise hijack audio from a Jabra/Dell headset the
    // user is actively on). Fails open if BLE is unavailable.
    bleWatcher.start();
    controller.setAvailabilityProbe([&bleWatcher]() {
        return bleWatcher.airpodsAvailable();
    });

    if (!tray.create(hInstance)) return 1;
    tray.setStatus(stateLabel(controller.state()));

    controller.setOnStateChanged([&tray](OwnershipState s) {
        tray.setStatus(stateLabel(s));
    });

    // Provide live status to the tray menu (queried each time the menu opens).
    tray.setStatusProvider([&]() -> TrayIcon::StatusInfo {
        TrayIcon::StatusInfo info;
        info.airpodsConnected = airpods.isClassicallyConnected();
        info.autoRouteEnabled = controller.autoRoute();
        for (auto& p : peers.peerInfos())
            info.peers.push_back({p.address, p.name, p.connected});
        return info;
    });

    peers.setOnPacket([&controller](std::span<const std::uint8_t> data) {
        controller.onIncomingPacket(data);
    });
    peers.setOnState([&controller](bool connected) {
        controller.onPeerConnectionChanged(connected);
    });

    media.setCallback([&controller](bool playing) {
        controller.onMediaPlayingChanged(playing);
    });

    // WASAPI-based trigger (Teams calls, VLC, …) feeds the handover path via
    // onCallActive(), which applies the voluntary-release gate in addition to
    // the standard debounce and anti-pingpong guards. This prevents an ongoing
    // Teams call from re-triggering a claim after Windows voluntarily released
    // AirPods to a peer.
    teamsWatcher.setOnTriggered([&controller]() {
        controller.onCallActive();
    });

    // When the Teams audio session expires (call ended), clear the voluntary-release
    // gate so the next new call can trigger a fresh claim.
    teamsWatcher.setOnSessionEnded([&controller]() {
        controller.onTeamsCallEnded();
    });

    tray.setMenuHandler([&](int cmd) {
        switch (cmd) {
            case TrayIcon::kCmdQuit:
                tray.postQuit();
                break;
            case TrayIcon::kCmdPairAndroid: {
                auto discovered = discoverAndroidPeers();
                auto& addrs = settings.androidAddresses;

                // Filter to devices not yet configured.
                std::vector<DiscoveredPeer> newPeers;
                for (auto& p : discovered) {
                    if (std::find(addrs.begin(), addrs.end(), p.address) == addrs.end())
                        newPeers.push_back(p);
                    else
                        log::info("Android peer {} already registered",
                            SettingsStore::formatBluetoothAddress(p.address));
                }

                if (newPeers.empty()) {
                    if (discovered.empty()) {
                        MessageBoxW(nullptr,
                            L"No paired Android devices found running Propods.\n\n"
                            L"Make sure:\n"
                            L"  \x2022 The phone is paired in Windows Bluetooth settings\n"
                            L"  \x2022 Cross-Device Handover is ON in the Propods Android app",
                            L"Propods \x2014 Add peer", MB_OK | MB_ICONINFORMATION);
                    } else {
                        MessageBoxW(nullptr,
                            L"All discovered devices are already added as peers.",
                            L"Propods \x2014 Add peer", MB_OK | MB_ICONINFORMATION);
                    }
                    break;
                }

                // Ask the user to confirm each new device.
                bool anyAdded = false;
                for (auto& p : newPeers) {
                    auto addrStr = SettingsStore::formatBluetoothAddress(p.address);
                    std::wstring msg = L"Add as Android peer?\n\n" + p.name
                        + L"\n" + std::wstring(addrStr.begin(), addrStr.end());
                    int res = MessageBoxW(nullptr, msg.c_str(),
                        L"Propods \x2014 Add peer", MB_YESNO | MB_ICONQUESTION);
                    if (res == IDYES) {
                        addrs.push_back(p.address);
                        peers.addPeer(p.address);
                        log::info("Added new Android peer: {} ({})",
                            to_string(p.name),
                            SettingsStore::formatBluetoothAddress(p.address));
                        anyAdded = true;
                    }
                }
                if (anyAdded) store.save(settings);
                break;
            }
            case TrayIcon::kCmdPairAirPods:
                if (auto a = discoverAirPods()) {
                    settings.airpodsAddress = a;
                    store.save(settings);
                    airpods.setAddress(*a);
                }
                break;
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
                if (cmd >= TrayIcon::kCmdRemovePeer) {
                    int idx = cmd - TrayIcon::kCmdRemovePeer;
                    auto& addrs = settings.androidAddresses;
                    if (idx >= 0 && idx < (int)addrs.size()) {
                        std::uint64_t addr = addrs[idx];
                        peers.removePeer(addr);
                        addrs.erase(addrs.begin() + idx);
                        store.save(settings);
                        log::info("Removed Android peer: {}",
                            SettingsStore::formatBluetoothAddress(addr));
                    }
                }
                break;
        }
    });

    for (auto addr : settings.androidAddresses)
        peers.addPeer(addr);

    if (!settings.androidAddresses.empty()) {
        peers.start();
    } else {
        log::warn("No Android peers configured. Use tray menu \"Add Android peer...\" once a device is paired in Windows.");
    }

    media.start();
    teamsWatcher.start();

    tray.runMessageLoop();

    teamsWatcher.stop();
    bleWatcher.stop();
    media.stop();
    peers.stop();
    return 0;
}
