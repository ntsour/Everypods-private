#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Networking.Sockets.h>

namespace Propods {

// Advertises the computer peer service (crossdevice::kComputerServiceUuid) so
// another computer (Windows or Mac) can open a coordination link to this PC.
// Each accepted link is handed to onConnection with the dialler's address.
class ComputerPeerServer {
public:
    using ConnectionCallback = std::function<void(std::uint64_t address,
        winrt::Windows::Networking::Sockets::StreamSocket socket)>;

    ~ComputerPeerServer() { stop(); }

    void setOnConnection(ConnectionCallback cb) { m_onConnection = std::move(cb); }
    bool start();
    void stop();

private:
    std::atomic<bool> m_running{false};
    ConnectionCallback m_onConnection;
    winrt::Windows::Devices::Bluetooth::Rfcomm::RfcommServiceProvider m_provider{nullptr};
    winrt::Windows::Networking::Sockets::StreamSocketListener m_listener{nullptr};
};

}
