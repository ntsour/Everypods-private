#pragma once

#include "BluetoothRfcommClient.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace Propods {

// Manages one BluetoothRfcommClient link per peer. Every peer is equal: all of
// them listen on crossdevice::kServiceUuid, we dial each configured peer there,
// and any of them may dial us. When both directions come up, the link dialled
// by the peer with the smaller installationId is kept (docs/peer-mesh-spec.md
// section 1.2, v2/PeerV2Arbitration.hpp).
// Presents the same logical interface as a single BluetoothRfcommClient:
//   - sendPacket()     → broadcast to every currently-connected peer
//   - isAnyConnected() → true if at least one peer is connected
//   - callbacks        → fired for each packet/state-change from any peer
//
// Thread-safe: addPeer / start / stop / sendPacket may be called from any thread.
class PeerRegistry {
public:
    using PacketCallback = std::function<void(std::span<const std::uint8_t>)>;
    using StateCallback  = std::function<void(bool connected)>;

    // A peer we did not know dialled us; it is now a peer (save it).
    using PeerAddedCallback = std::function<void(std::uint64_t address)>;

    // Snapshot of a single peer's status, used for UI display.
    struct PeerInfo {
        std::uint64_t address{};
        std::wstring  name;       // device display name, or formatted address if not yet known
        bool          connected{false};
    };

    void setOnPacket(PacketCallback cb);
    void setOnState(StateCallback  cb);
    void setOnPeerAdded(PeerAddedCallback cb);

    // Register a peer address. If start() has already been called the connection
    // loop is launched immediately; otherwise it starts on the next start() call.
    // Duplicate addresses are silently ignored.
    void addPeer(std::uint64_t address);

    // A peer dialled our legacy service. Unknown addresses are added as peers
    // (Bluetooth pairing is the trust boundary). If our own link to it is also up,
    // one of the two is closed (see above).
    void acceptInbound(std::uint64_t address,
                       winrt::Windows::Networking::Sockets::StreamSocket socket);

    // Stop and remove a peer by address. No-op if the address is not registered.
    void removePeer(std::uint64_t address);

    // Launch connection loops for all registered peers.
    void start();

    // Stop all connection loops and clear the peer list.
    void stop();

    // True if at least one peer is currently connected.
    bool isAnyConnected() const;

    // Send a packet to every currently-connected peer.
    void sendPacket(std::span<const std::uint8_t> bytes);

    // Returns a snapshot of registered peer addresses.
    std::vector<std::uint64_t> addresses() const;

    // Returns a snapshot of all peers with their current connection status and name.
    std::vector<PeerInfo> peerInfos() const;

private:
    struct Entry {
        std::uint64_t address{};
        std::string   name;   // last known BT display name (ASCII-safe for device names)
        std::unique_ptr<BluetoothRfcommClient> client;
    };

    void wireCallbacks(Entry& e);
    void startClient(BluetoothRfcommClient& client, std::uint64_t address) const;

    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
    PacketCallback m_onPacket;
    StateCallback  m_onState;
    PeerAddedCallback m_onPeerAdded;
    bool m_started{false};
};

} // namespace Propods
