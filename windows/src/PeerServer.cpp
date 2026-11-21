#include "PeerServer.hpp"

#include "BluetoothRfcommClient.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>

#include "Logger.hpp"
#include "crossdevice_protocol.hpp"

using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
using namespace winrt::Windows::Networking::Sockets;

namespace Propods {

bool PeerServer::start() {
    if (m_running.exchange(true)) return true;
    try {
        m_provider = RfcommServiceProvider::CreateAsync(
            RfcommServiceId::FromUuid(uuidFromString(crossdevice::kServiceUuid))).get();
        m_listener = StreamSocketListener{};
        m_listener.ConnectionReceived([this](StreamSocketListener const&,
                                             StreamSocketListenerConnectionReceivedEventArgs const& args) {
            auto socket = args.Socket();
            const auto address = remoteBluetoothAddress(socket);
            if (address == 0) {
                log::warn("Peer link: cannot read the caller's address; closing");
                try { socket.Close(); } catch (...) {}
                return;
            }
            log::info("Peer {:012X} opened a link to this PC", address);
            if (m_onConnection) m_onConnection(address, socket);
            else { try { socket.Close(); } catch (...) {} }
        });
        m_listener.BindServiceNameAsync(m_provider.ServiceId().AsString(),
            SocketProtectionLevel::BluetoothEncryptionWithAuthentication).get();
        m_provider.StartAdvertising(m_listener);
        log::info("Peer link service advertised (peers can dial this PC)");
        return true;
    } catch (const winrt::hresult_error& e) {
        log::warn("Peer link service failed to start: 0x{:08X} {}",
            static_cast<std::uint32_t>(e.code().value), winrt::to_string(e.message()));
        m_running.store(false);
        m_listener = nullptr;
        m_provider = nullptr;
        return false;
    }
}

void PeerServer::stop() {
    if (!m_running.exchange(false)) return;
    try { if (m_provider) m_provider.StopAdvertising(); } catch (...) {}
    try { if (m_listener) m_listener.Close(); } catch (...) {}
    m_provider = nullptr;
    m_listener = nullptr;
}

}
