#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <string_view>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>

namespace Propods {

// "1abbb9a4-10e4-4000-a75c-8953c5471342" -> GUID.
winrt::guid uuidFromString(std::string_view s);

// Bluetooth address of the device at the far end of an accepted socket
// ("(AA:BB:CC:DD:EE:FF)" in RawName), or 0 if it cannot be read.
std::uint64_t remoteBluetoothAddress(const winrt::Windows::Networking::Sockets::StreamSocket& socket);

// Dials the peer's RFCOMM service the way Android does (docs/peer-link.md,
// "Dial and retry"): resolves the service fresh every time (uncached, with one
// cached fallback), gives each step kPeerConnectTimeout (8 s) and cancels/closes
// on expiry, and holds one process-wide mutex so only one dial (legacy or v2)
// is in flight at a time. Blocks; throws on failure or timeout. `tag` is for
// logs. `onName`, if set, gets the device display name before the connect.
winrt::Windows::Networking::Sockets::StreamSocket dialRfcomm(
    std::uint64_t peerAddress, const winrt::guid& serviceUuid, const char* tag,
    const std::function<void(const std::string&)>& onName = {});

// One coordination link to one peer. It dials the peer's RFCOMM service and
// reconnects with backoff. It can also take over a link the peer dialled to
// us (adoptInbound), so either side may open it.
class BluetoothRfcommClient {
public:
    using PacketCallback = std::function<void(std::span<const std::uint8_t>)>;
    using StateCallback  = std::function<void(bool connected)>;
    // Fired once per successful connection with the BT device's display name.
    using NameCallback   = std::function<void(const std::string& name)>;

    BluetoothRfcommClient();
    ~BluetoothRfcommClient();

    BluetoothRfcommClient(const BluetoothRfcommClient&) = delete;
    BluetoothRfcommClient& operator=(const BluetoothRfcommClient&) = delete;

    void setOnPacket(PacketCallback cb) { m_onPacket = std::move(cb); }
    void setOnState(StateCallback  cb) { m_onState  = std::move(cb); }
    void setOnName (NameCallback   cb) { m_onName   = std::move(cb); }
    // The peer closed a link right after it came up (peerLinkTooShort): an
    // Android peer does that while this PC is not authorized there. Called on
    // the worker thread; set before start().
    using RejectedCallback = std::function<void()>;
    void setOnRejected(RejectedCallback cb) { m_onRejected = std::move(cb); }

    // Evaluated on the worker thread before the first dial: how long to wait so
    // the peer with the smaller installationId usually dials first.
    using DelayProvider = std::function<std::chrono::milliseconds()>;
    // Evaluated when a link we dialled and a link the peer dialled are both up:
    // true keeps the one we dialled, false keeps the peer's.
    using KeepOwnDialled = std::function<bool()>;

    // serviceUuid: the peer's service to dial (always kServiceUuid).
    void start(std::uint64_t peerAddress,
               std::string_view serviceUuid,
               DelayProvider firstDialDelay = {},
               KeepOwnDialled keepOwnDialled = {});
    void stop();

    bool isConnected() const { return m_connected.load(); }
    // True while the current link is one the peer dialled to us.
    bool isInbound() const { return m_inbound.load(); }

    // Use a link the peer dialled to us. If a link is already up it is closed
    // and replaced by this one.
    void adoptInbound(winrt::Windows::Networking::Sockets::StreamSocket socket);

    void sendPacket(std::span<const std::uint8_t> bytes);

    // The peer's Bluetooth link came up: cut the backoff wait short and dial at
    // once. Does not stop the client; no-op while a link is up.
    void retryNow();

private:
    void runLoop(std::uint64_t peerAddress);
    winrt::Windows::Networking::Sockets::StreamSocket dial(std::uint64_t peerAddress);
    void runConnection(std::uint64_t peerAddress,
                       winrt::Windows::Networking::Sockets::StreamSocket socket, bool inbound);
    winrt::Windows::Networking::Sockets::StreamSocket takePendingInbound();
    // Sleeps up to `d`; returns early when stopping or a peer link arrives, and,
    // if `interruptibleByRetry`, when retryNow() is called.
    void waitFor(std::chrono::milliseconds d, bool interruptibleByRetry);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_inbound{false};
    std::atomic<std::uint64_t> m_peerAddress{0};
    std::string m_serviceUuid;
    DelayProvider m_firstDialDelay;
    KeepOwnDialled m_keepOwnDialled;
    std::thread m_worker;

    std::mutex m_socketMutex;
    winrt::Windows::Networking::Sockets::StreamSocket m_socket{nullptr};
    winrt::Windows::Storage::Streams::DataWriter m_writer{nullptr};

    std::mutex m_pendingMutex;
    std::condition_variable m_pendingCv;
    winrt::Windows::Networking::Sockets::StreamSocket m_pendingInbound{nullptr};
    bool m_retryRequested{false};  // guarded by m_pendingMutex

    PacketCallback m_onPacket;
    StateCallback  m_onState;
    NameCallback   m_onName;
    RejectedCallback m_onRejected;
};

}
