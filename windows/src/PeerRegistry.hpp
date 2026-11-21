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

// Manages one BluetoothRfcommClient link per peer: Android phones and other
// computers (Windows or Mac). All peers listen on crossdevice::kServiceUuid and
// we dial them there, and any of them may dial us (a phone does when the user
// picks this PC as its peer). Kind is a label
// (UI, trace upload and the v2 mesh are phone-only).
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
    enum class Kind { Phone, Computer };

    // A peer we did not know dialled us; it is now a peer of that kind (save it).
    using PeerAddedCallback = std::function<void(std::uint64_t address, Kind kind)>;

    // Snapshot of a single peer's status, used for UI display.
    struct PeerInfo {
        std::uint64_t address{};
        std::wstring  name;       // device display name, or formatted address if not yet known
        bool          connected{false};
        Kind          kind{Kind::Phone};
    };

    void setOnPacket(PacketCallback cb);
    void setOnState(StateCallback  cb);
    void setOnPeerAdded(PeerAddedCallback cb);

    // This PC's Bluetooth address. Decides which link survives when this PC
    // and a computer peer dial each other at the same time.
    void setLocalAddress(std::uint64_t address);

    // Register a peer address. If start() has already been called the connection
    // loop is launched immediately; otherwise it starts on the next start() call.
    // Duplicate addresses are silently ignored.
    void addPeer(std::uint64_t address, Kind kind = Kind::Phone);

    // A peer dialled our peer service. `kind` comes from the device's Bluetooth
    // class. Unknown addresses are added with that kind (Bluetooth pairing is
    // the trust boundary); a known peer keeps its configured kind.
    void acceptInbound(std::uint64_t address, Kind kind,
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
    PeerAddedCallback m_onPeerAdded;
    std::uint64_t m_localAddress{0};
    bool m_started{false};
};

} // namespace Propods
