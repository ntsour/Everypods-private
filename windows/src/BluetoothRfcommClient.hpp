#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
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

    // serviceUuid: the peer's service to dial (always kServiceUuid, for
    // phones and computers alike). firstDialDelay: wait before the first dial, so
    // the side whose dial should win gets there first.
    void start(std::uint64_t peerAddress,
               std::string_view serviceUuid,
               std::chrono::milliseconds firstDialDelay = std::chrono::milliseconds{0});
    void stop();

    bool isConnected() const { return m_connected.load(); }
    // True while the current link is one the peer dialled to us.
    bool isInbound() const { return m_inbound.load(); }

    // Use a link the peer dialled to us. If a link is already up it is closed
    // and replaced by this one.
    void adoptInbound(winrt::Windows::Networking::Sockets::StreamSocket socket);

    void sendPacket(std::span<const std::uint8_t> bytes);

private:
    void runLoop(std::uint64_t peerAddress);
    winrt::Windows::Networking::Sockets::StreamSocket dial(std::uint64_t peerAddress);
    void runConnection(std::uint64_t peerAddress,
                       winrt::Windows::Networking::Sockets::StreamSocket socket, bool inbound);
    winrt::Windows::Networking::Sockets::StreamSocket takePendingInbound();
    // Sleeps up to `d`; returns early when stopping or a peer link arrives.
    void waitFor(std::chrono::milliseconds d);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_inbound{false};
    std::atomic<std::uint64_t> m_peerAddress{0};
    std::string m_serviceUuid;
    std::chrono::milliseconds m_firstDialDelay{0};
    std::thread m_worker;

    std::mutex m_socketMutex;
    winrt::Windows::Networking::Sockets::StreamSocket m_socket{nullptr};
    winrt::Windows::Storage::Streams::DataWriter m_writer{nullptr};

    std::mutex m_pendingMutex;
    std::condition_variable m_pendingCv;
    winrt::Windows::Networking::Sockets::StreamSocket m_pendingInbound{nullptr};

    PacketCallback m_onPacket;
    StateCallback  m_onState;
    NameCallback   m_onName;
};

}
