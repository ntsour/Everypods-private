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

// Manages one BluetoothRfcommClient link per peer: Android phones (we dial
// their CrossDevice service) and other computers (Windows or Mac; either side
// may dial, over the computer peer service).
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
    // A computer we did not know dialled us; it is now a peer (save it).
    using PeerAddedCallback = std::function<void(std::uint64_t address)>;

    enum class Kind { Phone, Computer };

    // Snapshot of a single peer's status, used for UI display.
    struct PeerInfo {
        std::uint64_t address{};
        std::wstring  name;       // device display name, or formatted address if not yet known
        bool          connected{false};
        Kind          kind{Kind::Phone};
    };

    void setOnPacket(PacketCallback cb);
    void setOnState(StateCallback  cb);
    void setOnComputerPeerAdded(PeerAddedCallback cb);

    // This PC's Bluetooth address. Decides which link survives when this PC
    // and a computer peer dial each other at the same time.
    void setLocalAddress(std::uint64_t address);

    // Register a peer address. If start() has already been called the connection
    // loop is launched immediately; otherwise it starts on the next start() call.
    // Duplicate addresses are silently ignored.
    void addPeer(std::uint64_t address, Kind kind = Kind::Phone);

    // A computer dialled our computer peer service. Unknown computers are
    // added as peers (Bluetooth pairing is the trust boundary).
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
        Kind          kind{Kind::Phone};
        std::string   name;   // last known BT display name (ASCII-safe for device names)
        std::unique_ptr<BluetoothRfcommClient> client;
    };

    void wireCallbacks(Entry& e);
    void startClient(BluetoothRfcommClient& client, std::uint64_t address, Kind kind) const;

    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
    PacketCallback m_onPacket;
    StateCallback  m_onState;
    PeerAddedCallback m_onComputerPeerAdded;
    std::uint64_t m_localAddress{0};
    bool m_started{false};
};

} // namespace Propods
