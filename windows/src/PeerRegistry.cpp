#include "PeerRegistry.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>

#include "Logger.hpp"
#include "SettingsStore.hpp"
#include "crossdevice_protocol.hpp"

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

void PeerRegistry::setLocalAddress(std::uint64_t address) {
    std::scoped_lock lk{m_mutex};
    m_localAddress = address;
}

void PeerRegistry::startClient(BluetoothRfcommClient& client, std::uint64_t address, Kind kind) const {
    // Computers always dial each other, so give the lower address a head start:
    // its link is the one kept when both get through. Phones dial us only when
    // the user picks this PC on the phone, so don't slow phone links down for it.
    std::chrono::milliseconds delay{0};
    if (kind == Kind::Computer) {
        std::uint64_t local;
        { std::scoped_lock lk{m_mutex}; local = m_localAddress; }
        if (local != 0 && local > address) delay = std::chrono::milliseconds{3000};
    }
    client.start(address, crossdevice::kServiceUuid, delay);
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

void PeerRegistry::addPeer(std::uint64_t address, Kind kind) {
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
            Entry{address, kind, /*name=*/{}, std::make_unique<BluetoothRfcommClient>()});
        wireCallbacks(e);
        shouldStart = m_started;
    }
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
            startClient(*clientPtr, address, kind);
            log::info("PeerRegistry: added and started {} peer {}",
                kind == Kind::Computer ? "computer" : "phone",
                SettingsStore::formatBluetoothAddress(address));
        }
    } else {
        log::info("PeerRegistry: registered peer {} (not yet started)",
            SettingsStore::formatBluetoothAddress(address));
    }
}

void PeerRegistry::acceptInbound(std::uint64_t address, Kind kind,
                                 winrt::Windows::Networking::Sockets::StreamSocket socket) {
    const auto label = SettingsStore::formatBluetoothAddress(address);
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
                Entry{address, kind, /*name=*/{}, std::make_unique<BluetoothRfcommClient>()});
            wireCallbacks(e);
            client = e.client.get();
            added = true;
            onAdded = m_onPeerAdded;
        } else if (client->isConnected() && !client->isInbound() && m_localAddress != 0 &&
                   m_localAddress < address) {
            // Both sides dialled. The lower address's link wins; that is ours.
            keepOurs = true;
        }
    }
    if (keepOurs) {
        log::info("PeerRegistry: peer {} dialled while our link to it is up; keeping ours", label);
        try { socket.Close(); } catch (...) {}
        return;
    }
    if (added) {
        startClient(*client, address, kind);
        log::info("PeerRegistry: {} {} dialled us; added it as a peer",
            kind == Kind::Computer ? "computer" : "phone", label);
    }
    client->adoptInbound(socket);
    if (added && onAdded) onAdded(address, kind);
}

void PeerRegistry::removePeer(std::uint64_t address) {
    std::unique_ptr<BluetoothRfcommClient> toStop;
    {
        std::scoped_lock lk{m_mutex};
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->address == address) {
                toStop = std::move(it->client);
                m_entries.erase(it);
                break;
            }
        }
    }
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
    struct Pending { std::uint64_t address; Kind kind; BluetoothRfcommClient* client; };
    std::vector<Pending> toStart;
    {
        std::scoped_lock lk{m_mutex};
        if (m_started) return;
        m_started = true;
        for (auto& e : m_entries)
            toStart.push_back({e.address, e.kind, e.client.get()});
    }
    for (auto& p : toStart)
        startClient(*p.client, p.address, p.kind);
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
    for (auto& e : toStop)
        e.client->stop();
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
        info.kind      = e.kind;
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
