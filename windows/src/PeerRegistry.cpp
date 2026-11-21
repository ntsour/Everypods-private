#include "PeerRegistry.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>

#include "Logger.hpp"
#include "SettingsStore.hpp"
#include "crossdevice_protocol.hpp"
#include "v2/PeerV2Arbitration.hpp"
#include "v2/PeerV2Identity.hpp"

namespace Propods {

void PeerRegistry::setOnPacket(PacketCallback cb) {
    std::scoped_lock lk{m_mutex};
    m_onPacket = std::move(cb);
}

void PeerRegistry::setOnState(StateCallback cb) {
    std::scoped_lock lk{m_mutex};
    m_onState = std::move(cb);
}

void PeerRegistry::setOnPeerAdded(PeerAddedCallback cb) {
    std::scoped_lock lk{m_mutex};
    m_onPeerAdded = std::move(cb);
}

void PeerRegistry::setOnPeerLinkUp(PeerLinkUpCallback cb) {
    std::scoped_lock lk{m_linkUp->mutex};
    m_linkUp->callback = std::move(cb);
}

void PeerRegistry::revokeWatch(Entry& e) {
    // Called with no registry lock held.
    if (e.device && e.statusToken) {
        try { e.device.ConnectionStatusChanged(e.statusToken); } catch (...) {}
    }
    e.statusToken = {};
    e.device = nullptr;
}

// Android CrossDeviceClient.retryNow on ACL_CONNECTED: when the peer's Bluetooth
// link comes up, dial at once instead of waiting out the backoff.
void PeerRegistry::watchBluetoothDevice(std::uint64_t address) {
    std::weak_ptr<BluetoothRfcommClient> weakClient;
    {
        std::scoped_lock lk{m_mutex};
        for (auto& e : m_entries)
            if (e.address == address) { weakClient = e.client; break; }
    }
    if (weakClient.expired()) return;
    auto slot = m_linkUp;
    auto op = winrt::Windows::Devices::Bluetooth::BluetoothDevice::FromBluetoothAddressAsync(address);
    op.Completed([this, address, weakClient, slot](
            winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Bluetooth::BluetoothDevice> const& done,
            winrt::Windows::Foundation::AsyncStatus status) {
        using namespace winrt::Windows::Devices::Bluetooth;
        if (status != winrt::Windows::Foundation::AsyncStatus::Completed) return;
        BluetoothDevice device{nullptr};
        try { device = done.GetResults(); } catch (...) { return; }
        if (!device) return;  // not paired
        // Handlers capture no registry state: only the client and the slot.
        winrt::event_token token = device.ConnectionStatusChanged(
            [address, weakClient, slot](BluetoothDevice const& sender,
                                        winrt::Windows::Foundation::IInspectable const&) {
                try {
                    if (sender.ConnectionStatus() != BluetoothConnectionStatus::Connected) return;
                } catch (...) { return; }
                auto client = weakClient.lock();
                if (!client || client->isConnected()) return;
                log::info("PeerRegistry: Bluetooth link to {} is up",
                    SettingsStore::formatBluetoothAddress(address));
                client->retryNow();
                PeerLinkUpCallback cb;
                { std::scoped_lock lk{slot->mutex}; cb = slot->callback; }
                if (cb) cb(address);
            });
        bool kept = false;
        {
            std::scoped_lock lk{m_mutex};
            for (auto& e : m_entries) {
                if (e.address == address && !e.device) {
                    e.device = device;
                    e.statusToken = token;
                    kept = true;
                    break;
                }
            }
        }
        if (!kept) {  // the peer was removed meanwhile
            try { device.ConnectionStatusChanged(token); } catch (...) {}
        }
    });
}

void PeerRegistry::startClient(BluetoothRfcommClient& client, std::uint64_t address) const {
    // The peer with the smaller installationId dials at once; the other waits 3 s
    // before its first dial and then dials anyway, so a link forms even when the
    // other side never dials us. Ids are read on the worker thread, when it is
    // time to dial; an unknown peer id means no delay.
    client.start(address, crossdevice::kServiceUuid,
        [address] {
            return v2::firstDialDelay(v2::localIdentity().installationId,
                                      v2::knownPeerInstallationId(address));
        },
        [address] {
            // Both links are up and the peer's came up first: keep ours only if
            // both ids are known and ours is the smaller.
            return v2::chooseLegacyLink(v2::localIdentity().installationId,
                                        v2::knownPeerInstallationId(address),
                                        /*existingDialledByUs=*/false) == v2::LegacyLinkChoice::UseNew;
        });
}

void PeerRegistry::wireCallbacks(Entry& e) {
    std::uint64_t addr = e.address;  // capture address by value; Entry may be moved later

    e.client->setOnPacket([this](std::span<const std::uint8_t> data) {
        PacketCallback cb;
        { std::scoped_lock lk{m_mutex}; cb = m_onPacket; }
        if (cb) cb(data);
    });
    e.client->setOnState([this](bool connected) {
        StateCallback cb;
        { std::scoped_lock lk{m_mutex}; cb = m_onState; }
        if (cb) cb(connected);
    });
    // Store the device display name the first time we successfully connect.
    e.client->setOnName([this, addr](const std::string& name) {
        std::scoped_lock lk{m_mutex};
        for (auto& entry : m_entries)
            if (entry.address == addr) { entry.name = name; break; }
    });
}

void PeerRegistry::addPeer(std::uint64_t address) {
    bool shouldStart = false;
    {
        std::scoped_lock lk{m_mutex};
        for (auto& e : m_entries) {
            if (e.address == address) {
                log::warn("PeerRegistry: peer {} already registered — skipping",
                    SettingsStore::formatBluetoothAddress(address));
                return;
            }
        }
        auto& e = m_entries.emplace_back(
            Entry{address, /*name=*/{}, std::make_shared<BluetoothRfcommClient>()});
        wireCallbacks(e);
        shouldStart = m_started;
    }
    watchBluetoothDevice(address);
    if (shouldStart) {
        // Find the entry again outside the lock to call start() without holding it.
        // (BluetoothRfcommClient::start launches a thread; holding m_mutex during
        // thread launch risks lock-order inversion with the client's internal mutex.)
        BluetoothRfcommClient* clientPtr = nullptr;
        {
            std::scoped_lock lk{m_mutex};
            for (auto& e : m_entries) {
                if (e.address == address) { clientPtr = e.client.get(); break; }
            }
        }
        if (clientPtr) {
            startClient(*clientPtr, address);
            log::info("PeerRegistry: added and started peer {}",
                SettingsStore::formatBluetoothAddress(address));
        }
    } else {
        log::info("PeerRegistry: registered peer {} (not yet started)",
            SettingsStore::formatBluetoothAddress(address));
    }
}

void PeerRegistry::acceptInbound(std::uint64_t address,
                                 winrt::Windows::Networking::Sockets::StreamSocket socket) {
    const auto label = SettingsStore::formatBluetoothAddress(address);
    const std::string ownId = v2::localIdentity().installationId;
    const std::string peerId = v2::knownPeerInstallationId(address);
    BluetoothRfcommClient* client = nullptr;
    bool added = false;
    bool keepOurs = false;
    PeerAddedCallback onAdded;
    {
        std::scoped_lock lk{m_mutex};
        if (!m_started) {
            log::info("PeerRegistry: peer {} dialled before startup finished; closing", label);
            try { socket.Close(); } catch (...) {}
            return;
        }
        for (auto& e : m_entries) {
            if (e.address != address) continue;
            client = e.client.get();
            break;
        }
        if (!client) {
            auto& e = m_entries.emplace_back(
                Entry{address, /*name=*/{}, std::make_shared<BluetoothRfcommClient>()});
            wireCallbacks(e);
            client = e.client.get();
            added = true;
            onAdded = m_onPeerAdded;
        } else if (client->isConnected() && !client->isInbound()) {
            // Both sides dialled and our link is already up. Keep the link dialled
            // by the smaller installationId; with unknown ids keep ours (first up).
            keepOurs = v2::chooseLegacyLink(ownId, peerId, /*existingDialledByUs=*/true) ==
                       v2::LegacyLinkChoice::KeepExisting;
        }
    }
    if (keepOurs) {
        log::info("PeerRegistry: peer {} dialled while our link to it is up; keeping ours", label);
        try { socket.Close(); } catch (...) {}
        return;
    }
    if (added) {
        watchBluetoothDevice(address);
        startClient(*client, address);
        log::info("PeerRegistry: {} dialled us; added it as a peer", label);
    }
    client->adoptInbound(socket);
    if (added && onAdded) onAdded(address);
}

void PeerRegistry::removePeer(std::uint64_t address) {
    std::shared_ptr<BluetoothRfcommClient> toStop;
    Entry removed;
    {
        std::scoped_lock lk{m_mutex};
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->address == address) {
                toStop = it->client;
                removed = std::move(*it);
                m_entries.erase(it);
                break;
            }
        }
    }
    revokeWatch(removed);
    if (toStop) {
        toStop->stop();
        log::info("PeerRegistry: removed peer {}",
            SettingsStore::formatBluetoothAddress(address));
    } else {
        log::warn("PeerRegistry: removePeer({}) — not found",
            SettingsStore::formatBluetoothAddress(address));
    }
}

void PeerRegistry::start() {
    // Collect (address, client*) pairs without holding the lock during start().
    struct Pending { std::uint64_t address; BluetoothRfcommClient* client; };
    std::vector<Pending> toStart;
    {
        std::scoped_lock lk{m_mutex};
        if (m_started) return;
        m_started = true;
        for (auto& e : m_entries)
            toStart.push_back({e.address, e.client.get()});
    }
    for (auto& p : toStart)
        startClient(*p.client, p.address);
    log::info("PeerRegistry: started {} peer(s)", toStart.size());
}

void PeerRegistry::stop() {
    // Move entries out of the registry under the lock so isAnyConnected /
    // sendPacket see an empty list immediately. Then stop the clients outside
    // the lock to avoid deadlock: BluetoothRfcommClient::stop() joins the worker
    // thread, whose state-callback tries to acquire m_mutex.
    std::vector<Entry> toStop;
    {
        std::scoped_lock lk{m_mutex};
        m_started = false;
        toStop = std::move(m_entries);
        m_entries.clear();
    }
    for (auto& e : toStop) {
        revokeWatch(e);
        e.client->stop();
    }
    log::info("PeerRegistry: stopped {} peer(s)", toStop.size());
}

bool PeerRegistry::isAnyConnected() const {
    std::scoped_lock lk{m_mutex};
    for (auto& e : m_entries)
        if (e.client->isConnected()) return true;
    return false;
}

void PeerRegistry::sendPacket(std::span<const std::uint8_t> bytes) {
    std::scoped_lock lk{m_mutex};
    for (auto& e : m_entries)
        if (e.client->isConnected())
            e.client->sendPacket(bytes);
}

std::vector<std::uint64_t> PeerRegistry::addresses() const {
    std::scoped_lock lk{m_mutex};
    std::vector<std::uint64_t> result;
    result.reserve(m_entries.size());
    for (auto& e : m_entries) result.push_back(e.address);
    return result;
}

std::vector<PeerRegistry::PeerInfo> PeerRegistry::peerInfos() const {
    std::scoped_lock lk{m_mutex};
    std::vector<PeerInfo> result;
    result.reserve(m_entries.size());
    for (auto& e : m_entries) {
        PeerInfo info;
        info.address   = e.address;
        info.connected = e.client->isConnected();
        if (!e.name.empty()) {
            // Device names are ASCII-safe; widen char-by-char.
            info.name.assign(e.name.begin(), e.name.end());
        } else {
            // Fallback: formatted BT address until first successful connection.
            auto s = SettingsStore::formatBluetoothAddress(e.address);
            info.name.assign(s.begin(), s.end());
        }
        result.push_back(std::move(info));
    }
    return result;
}

} // namespace Propods
