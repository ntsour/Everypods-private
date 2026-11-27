#include "BluetoothRfcommClient.hpp"

#include "LinkStats.hpp"
#include "Logger.hpp"
#include "PeerDialPolicy.hpp"
#include "crossdevice_protocol.hpp"

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Foundation;

namespace Propods {

namespace {

// One dial at a time across every peer and both services (Android
// PeerConnectionSupervisor.connectSerialized).
std::mutex g_dialMutex;

// Waits up to kPeerConnectTimeout for `op`; on expiry cancels it and throws.
template <typename Op>
void waitOrCancel(const Op& op, const char* what) {
    const winrt::Windows::Foundation::TimeSpan limit{kPeerConnectTimeout};
    if (op.wait_for(limit) == AsyncStatus::Started) {
        try { op.Cancel(); } catch (...) {}
        throw std::runtime_error(std::string(what) + " timed out");
    }
}

}

winrt::guid uuidFromString(std::string_view s) {
    winrt::guid g{};
    // Format: 1abbb9a4-10e4-4000-a75c-8953c5471342
    auto hex = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return (unsigned)(c - '0');
        if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return (unsigned)(c - 'A' + 10);
        return 0;
    };
    auto byte = [&](size_t i) -> std::uint8_t {
        return (std::uint8_t)((hex(s[i]) << 4) | hex(s[i + 1]));
    };
    g.Data1 = (std::uint32_t)byte(0) << 24 | (std::uint32_t)byte(2) << 16
            | (std::uint32_t)byte(4) <<  8 | (std::uint32_t)byte(6);
    g.Data2 = (std::uint16_t)((byte(9) << 8) | byte(11));
    g.Data3 = (std::uint16_t)((byte(14) << 8) | byte(16));
    g.Data4[0] = byte(19);
    g.Data4[1] = byte(21);
    g.Data4[2] = byte(24);
    g.Data4[3] = byte(26);
    g.Data4[4] = byte(28);
    g.Data4[5] = byte(30);
    g.Data4[6] = byte(32);
    g.Data4[7] = byte(34);
    return g;
}


BluetoothRfcommClient::BluetoothRfcommClient() = default;

BluetoothRfcommClient::~BluetoothRfcommClient() {
    stop();
}

std::uint64_t remoteBluetoothAddress(const StreamSocket& socket) {
    try {
        const auto raw = winrt::to_string(socket.Information().RemoteHostName().RawName());
        std::string hex;
        for (char c : raw) if (std::isxdigit(static_cast<unsigned char>(c))) hex += c;
        if (hex.size() == 12) return std::strtoull(hex.c_str(), nullptr, 16);
    } catch (...) {}
    return 0;
}

void BluetoothRfcommClient::start(std::uint64_t peerAddress, std::string_view serviceUuid,
                                  DelayProvider firstDialDelay, KeepOwnDialled keepOwnDialled) {
    if (m_running.exchange(true)) return;
    m_peerAddress.store(peerAddress);
    m_serviceUuid = std::string(serviceUuid);
    m_firstDialDelay = std::move(firstDialDelay);
    m_keepOwnDialled = std::move(keepOwnDialled);
    m_worker = std::thread([this, peerAddress]() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        runLoop(peerAddress);
    });
}

void BluetoothRfcommClient::stop() {
    if (!m_running.exchange(false)) return;
    {
        std::scoped_lock lk{m_socketMutex};
        if (m_socket) {
            try { m_socket.Close(); } catch (...) {}
            m_socket = nullptr;
            m_writer = nullptr;
        }
    }
    {
        std::scoped_lock lk{m_pendingMutex};
        if (m_pendingInbound) { try { m_pendingInbound.Close(); } catch (...) {} }
        m_pendingInbound = nullptr;
    }
    m_pendingCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void BluetoothRfcommClient::adoptInbound(StreamSocket socket) {
    if (!m_running.load()) { try { socket.Close(); } catch (...) {} return; }
    {
        std::scoped_lock lk{m_pendingMutex};
        if (m_pendingInbound) { try { m_pendingInbound.Close(); } catch (...) {} }
        m_pendingInbound = socket;
    }
    // Close the current link (if any) so the worker picks up the new one now.
    {
        std::scoped_lock lk{m_socketMutex};
        if (m_socket) {
            log::info("RFCOMM {:012X}: replacing the current link with the peer's", m_peerAddress.load());
            try { m_socket.Close(); } catch (...) {}
        }
    }
    m_pendingCv.notify_all();
}

StreamSocket BluetoothRfcommClient::takePendingInbound() {
    std::scoped_lock lk{m_pendingMutex};
    auto s = m_pendingInbound;
    m_pendingInbound = nullptr;
    return s;
}

void BluetoothRfcommClient::waitFor(std::chrono::milliseconds d, bool interruptibleByRetry) {
    std::unique_lock lk{m_pendingMutex};
    m_pendingCv.wait_for(lk, d, [this, interruptibleByRetry] {
        return !m_running.load() || m_pendingInbound != nullptr
            || (interruptibleByRetry && m_retryRequested);
    });
    if (interruptibleByRetry) m_retryRequested = false;
}

void BluetoothRfcommClient::retryNow() {
    if (!m_running.load() || m_connected.load()) return;
    log::info("RFCOMM {:012X}: peer Bluetooth link is up; dialling now", m_peerAddress.load());
    {
        std::scoped_lock lk{m_pendingMutex};
        m_retryRequested = true;
    }
    m_pendingCv.notify_all();
}

void BluetoothRfcommClient::sendPacket(std::span<const std::uint8_t> bytes) {
    std::scoped_lock lk{m_socketMutex};
    if (!m_writer) {
        log::warn("RFCOMM {:012X} not connected; dropping send of {} bytes", m_peerAddress.load(), bytes.size());
        return;
    }
    try {
        std::string hex;
        for (auto b : bytes) { char tmp[4]; std::snprintf(tmp, sizeof(tmp), "%02X ", b); hex += tmp; }
        log::debug("RFCOMM {:012X} send {} bytes: {}", m_peerAddress.load(), bytes.size(), hex);
        m_writer.WriteBytes(winrt::array_view<const std::uint8_t>(bytes.data(), bytes.data() + bytes.size()));
        m_writer.StoreAsync().get();
        stats::LinkStats::instance().frameOut(m_peerAddress.load(), stats::Channel::Legacy, bytes.size());
    } catch (const winrt::hresult_error& e) {
        log::warn("RFCOMM {:012X} send failed: 0x{:08X} {}", m_peerAddress.load(), (std::uint32_t)e.code().value, winrt::to_string(e.message()));
    }
}

StreamSocket dialRfcomm(std::uint64_t peerAddress, const winrt::guid& serviceUuid, const char* tag,
                        const std::function<void(const std::string&)>& onName) {
    // Only one dial at a time, across every peer and both services.
    std::scoped_lock dialLock{g_dialMutex};
    log::info("{} {:012X}: connecting...", tag, peerAddress);

    auto deviceOp = BluetoothDevice::FromBluetoothAddressAsync(peerAddress);
    waitOrCancel(deviceOp, "BluetoothDevice lookup");
    auto device = deviceOp.GetResults();
    if (!device) {
        throw std::runtime_error("BluetoothDevice not found (is it paired?)");
    }
    std::string deviceName = to_string(device.Name());
    log::debug("{} {:012X}: BluetoothDevice name='{}' connectionStatus={}",
        tag, peerAddress, deviceName, (int)device.ConnectionStatus());
    if (onName) onName(deviceName);

    // Resolve the service fresh every dial (a cached record can point at a
    // stale channel after the peer restarts); fall back to the cache once.
    auto rfcommId = RfcommServiceId::FromUuid(serviceUuid);
    RfcommDeviceService service{nullptr};
    // True when the device answered the fresh lookup without our service: it is
    // in range, and EveryPods is not listening there. An unreachable device
    // reports an error instead and gets the normal retry.
    bool answeredWithoutService = false;
    try {
        auto op = device.GetRfcommServicesForIdAsync(rfcommId, BluetoothCacheMode::Uncached);
        waitOrCancel(op, "RFCOMM service lookup (uncached)");
        auto result = op.GetResults();
        answeredWithoutService = result.Error() == BluetoothError::Success && result.Services().Size() == 0;
        if (result.Services().Size() > 0) {
            service = result.Services().GetAt(0);
            log::info("{} {:012X}: service resolved (uncached)", tag, peerAddress);
        }
    } catch (const winrt::hresult_error& e) {
        log::warn("{} {:012X}: uncached service lookup failed: 0x{:08X} {}", tag, peerAddress,
            (std::uint32_t)e.code().value, winrt::to_string(e.message()));
    } catch (const std::exception& e) {
        log::warn("{} {:012X}: uncached service lookup failed: {}", tag, peerAddress, e.what());
    }
    if (!service) {
        auto op = device.GetRfcommServicesForIdAsync(rfcommId, BluetoothCacheMode::Cached);
        waitOrCancel(op, "RFCOMM service lookup (cached)");
        auto result = op.GetResults();
        if (result.Services().Size() > 0) {
            service = result.Services().GetAt(0);
            log::info("{} {:012X}: service resolved (cached fallback)", tag, peerAddress);
        }
    }
    if (!service) {
        if (answeredWithoutService)
            throw PeerServiceMissing(
                "RFCOMM service not advertised -- is EveryPods running on that device?");
        throw std::runtime_error("RFCOMM service not found (device did not answer the lookup)");
    }
    log::debug("{} {:012X}: connecting to host='{}' service='{}'", tag, peerAddress,
        to_string(service.ConnectionHostName().DisplayName()),
        to_string(service.ConnectionServiceName()));

    StreamSocket socket;
    auto connectOp = socket.ConnectAsync(service.ConnectionHostName(), service.ConnectionServiceName());
    try {
        waitOrCancel(connectOp, "RFCOMM connect");
        connectOp.GetResults();
    } catch (...) {
        try { socket.Close(); } catch (...) {}
        throw;
    }
    return socket;
}

StreamSocket BluetoothRfcommClient::dial(std::uint64_t peerAddress) {
    try {
        StreamSocket socket = dialRfcomm(peerAddress, uuidFromString(m_serviceUuid), "RFCOMM",
            [this](const std::string& name) { if (m_onName) m_onName(name); });
        stats::LinkStats::instance().dial(peerAddress, stats::Channel::Legacy, true);
        return socket;
    } catch (...) {
        stats::LinkStats::instance().dial(peerAddress, stats::Channel::Legacy, false);
        throw;
    }
}

void BluetoothRfcommClient::runConnection(std::uint64_t peerAddress, StreamSocket socket, bool inbound) {
    DataWriter writer{socket.OutputStream()};
    DataReader reader{socket.InputStream()};
    reader.InputStreamOptions(InputStreamOptions::Partial);

    {
        std::scoped_lock lk{m_socketMutex};
        m_socket = socket;
        m_writer = writer;
    }
    m_inbound.store(inbound);
    m_connected.store(true);
    if (m_onState) m_onState(true);
    log::info("RFCOMM connected to {:012X} ({})", peerAddress, inbound ? "peer dialled us" : "we dialled");

    std::vector<std::uint8_t> buf;
    buf.resize(1024);
    crossdevice::StreamDecoder decoder;  // fresh per connection

    while (m_running.load()) {
        std::uint32_t read = reader.LoadAsync(1024).get();
        if (read == 0) {
            throw std::runtime_error("Peer closed connection");
        }
        if (buf.size() < read) buf.resize(read);
        reader.ReadBytes(winrt::array_view<std::uint8_t>(buf.data(), buf.data() + read));
        // Hex-dump incoming packets for debugging.
        if (read <= 64) {
            std::string hex;
            hex.reserve(read * 3);
            for (std::uint32_t i = 0; i < read; ++i) {
                char tmp[4];
                std::snprintf(tmp, sizeof(tmp), "%02X ", buf[i]);
                hex += tmp;
            }
            log::debug("RFCOMM {:012X} recv {} bytes: {}", peerAddress, read, hex);
        } else {
            log::debug("RFCOMM {:012X} recv {} bytes", peerAddress, read);
        }
        for (const auto& packet : decoder.feed(std::span<const std::uint8_t>(buf.data(), read))) {
            stats::LinkStats::instance().frameIn(peerAddress, stats::Channel::Legacy, packet.size());
            if (m_onPacket) m_onPacket(std::span<const std::uint8_t>(packet.data(), packet.size()));
        }
    }
}

void BluetoothRfcommClient::runLoop(std::uint64_t peerAddress) {
    int attempt = 0;  // failed dials since the last link (Android retryAttempt)
    int misses = 0;   // dials in a row that found no EveryPods service, or links
                      // the peer closed at once (peerLinkTooShort)
    bool shortLink = false;  // the last miss was a link closed at once
    if (m_firstDialDelay) {
        std::chrono::milliseconds delay{0};
        try { delay = m_firstDialDelay(); } catch (...) {}
        if (delay.count() > 0) {
            log::info("RFCOMM {:012X}: waiting {} ms before the first dial (the peer dials first)",
                peerAddress, delay.count());
            waitFor(delay, /*interruptibleByRetry=*/false);
        }
    }
    {   // A retryNow() during the first-dial wait must not cut it short.
        std::scoped_lock lk{m_pendingMutex};
        m_retryRequested = false;
    }

    while (m_running.load()) {
        try {
            bool inbound = true;
            StreamSocket socket = takePendingInbound();
            if (!socket) {
                inbound = false;
                socket = dial(peerAddress);
                // A link from the peer may have arrived while we were dialling.
                // Both links are up: keep the one dialled by the peer with the
                // smaller installationId; unknown ids keep the one that came up
                // first, which is theirs (it arrived while we were still dialling).
                if (auto theirs = takePendingInbound()) {
                    const bool keepOurs = m_keepOwnDialled && m_keepOwnDialled();
                    if (keepOurs) {
                        log::info("RFCOMM {:012X}: both links up; keeping the one we dialled", peerAddress);
                        try { theirs.Close(); } catch (...) {}
                    } else {
                        try { socket.Close(); } catch (...) {}
                        socket = theirs;
                        inbound = true;
                    }
                }
            }
            attempt = 0;
            {   // A wake-up that arrived while dialling is spent now that a link is up.
                std::scoped_lock lk{m_pendingMutex};
                m_retryRequested = false;
            }
            const auto linkUp = std::chrono::steady_clock::now();
            stats::LinkStats::instance().linkUp(peerAddress, stats::Channel::Legacy);
            const auto lasted = [&] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - linkUp);
            };
            const auto settle = [&] {
                stats::LinkStats::instance().linkDown(peerAddress, stats::Channel::Legacy, lasted());
                if (peerLinkTooShort(lasted())) {
                    ++misses;
                    shortLink = true;
                    if (m_onRejected) { try { m_onRejected(); } catch (...) {} }
                } else {
                    misses = 0;
                    shortLink = false;
                }
            };
            try {
                runConnection(peerAddress, socket, inbound);
            } catch (...) {
                settle();
                throw;
            }
            settle();
        } catch (const PeerServiceMissing& e) {
            ++misses;
            shortLink = false;
            log::warn("RFCOMM {:012X} connection error: {}", peerAddress, e.what());
        } catch (const winrt::hresult_error& e) {
            if (!shortLink) misses = 0;
            log::warn("RFCOMM {:012X} connection error: 0x{:08X} {}", peerAddress,
                (std::uint32_t)e.code().value, winrt::to_string(e.message()));
        } catch (const std::exception& e) {
            if (!shortLink) misses = 0;
            log::warn("RFCOMM {:012X} connection error: {}", peerAddress, e.what());
        }

        {
            std::scoped_lock lk{m_socketMutex};
            if (m_socket) {
                try { m_socket.Close(); } catch (...) {}
                m_socket = nullptr;
                m_writer = nullptr;
            }
        }
        m_inbound.store(false);
        const bool wasConnected = m_connected.exchange(false);
        if (wasConnected && m_onState) m_onState(false);

        if (!m_running.load()) break;
        {
            // A replacement link from the peer is waiting: use it right away.
            std::scoped_lock lk{m_pendingMutex};
            if (m_pendingInbound) continue;
        }
        const auto backoff = misses > 0 ? peerServiceMissingDelay(misses, peerRetryJitter())
                                        : peerRetryDelay(attempt, peerRetryJitter());
        ++attempt;
        log::info("RFCOMM {:012X}: reconnecting in {} ms (attempt {}{})...", peerAddress, backoff.count(), attempt,
            misses == 0 ? "" : shortLink ? ", the peer closed the link at once" : ", no EveryPods service there");
        // The link-up wake does not end a wait for a missing service or a peer
        // that closes at once: our own dials bring that link up.
        waitFor(backoff, /*interruptibleByRetry=*/misses == 0);
        if (misses == 0) shortLink = false;
    }
}

}
