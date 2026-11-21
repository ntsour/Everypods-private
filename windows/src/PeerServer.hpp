#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Networking.Sockets.h>

namespace Propods {

// Advertises the legacy coordination service (crossdevice::kServiceUuid) so any
// peer can open a link to this PC. Each accepted link is handed to onConnection
// with the dialler's address. (The v2 service is served by v2::PeerV2Mesh.)
class PeerServer {
public:
    using ConnectionCallback = std::function<void(std::uint64_t address,
        winrt::Windows::Networking::Sockets::StreamSocket socket)>;

    ~PeerServer() { stop(); }

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
