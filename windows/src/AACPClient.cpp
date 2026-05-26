// winsock2.h MUST be included before any other Windows headers.
#include <winsock2.h>
#include <ws2bth.h>

#include "AACPClient.hpp"
#include "Logger.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <thread>

namespace Propods {

namespace {

constexpr ULONG kAACPPsm      = 0x1001;
constexpr int   kMaxRetries   = 4;
constexpr DWORD kHandshakeTimeoutMs = 2000;
constexpr DWORD kWatchdogMs   = 45000;

// Sent immediately after the L2CAP socket connects.
// Without this the AirPods won't send any packets back.
constexpr uint8_t kHandshake[] = {
    0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Subscribe to battery, ear-detection, stem press, etc.
constexpr uint8_t kEnableNotifications[] = {
    0x04, 0x00, 0x04, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFE, 0xFF
};

constexpr uint8_t kAACPHeader[]     = { 0x04, 0x00, 0x04, 0x00 };
constexpr int     kOpcodeOffset     = 4;
constexpr uint8_t kStemPressOpcode  = 0x19;
constexpr int     kStemPressMinLen  = 8; // header(4) + opcode(1) + pad(1) + type(1) + bud(1)

// Retry delays in ms: 1s, 2s, 4s, 8s
constexpr int kBackoffMs[kMaxRetries] = { 1000, 2000, 4000, 8000 };

} // anonymous namespace

AACPClient::AACPClient() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

AACPClient::~AACPClient() {
    stop();
    if (m_worker.joinable()) m_worker.join();
    WSACleanup();
}

void AACPClient::start(std::uint64_t address) {
    if (address == 0) return;

    // Signal any existing worker to stop and close its socket.
    m_running.store(false);
    auto old = m_socket.exchange(kInvalidSocket);
    if (old != kInvalidSocket) closesocket(static_cast<SOCKET>(old));

    // Wait for the old worker before creating a new thread.
    if (m_worker.joinable()) m_worker.join();

    m_running.store(true);
    m_worker = std::thread([this, address] { runLoop(address); });
}

void AACPClient::stop() {
    m_running.store(false);
    auto sock = m_socket.exchange(kInvalidSocket);
    if (sock != kInvalidSocket) closesocket(static_cast<SOCKET>(sock));
    // Worker thread exits asynchronously; ~AACPClient / next start() will join it.
}

void AACPClient::runLoop(std::uint64_t address) {
    int attempt = 0;

    while (m_running.load()) {
        log::info("AACP    Connect attempt {}/{}", attempt + 1, kMaxRetries + 1);

        if (connectAndHandshake(address)) {
            attempt = 0;  // reset backoff on successful connect
            m_connected.store(true);
            log::info("AACP    Handshake OK — listening for stem presses");
            readLoop();
            m_connected.store(false);
            log::info("AACP    Read loop ended");
        }

        if (!m_running.load()) break;

        if (attempt < kMaxRetries) {
            int delay = kBackoffMs[attempt++];
            log::debug("AACP    Retry in {}ms (attempt {})", delay, attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        } else {
            log::warn("AACP    Max retries ({}) reached — stopping", kMaxRetries);
            break;
        }
    }

    m_connected.store(false);
}

bool AACPClient::connectAndHandshake(std::uint64_t address) {
    SOCKET sock = socket(AF_BTH, SOCK_SEQPACKET, BTHPROTO_L2CAP);
    if (sock == INVALID_SOCKET) {
        log::error("AACP    socket() failed: {}", WSAGetLastError());
        return false;
    }

    // Short timeout for the handshake exchange.
    DWORD timeout = kHandshakeTimeoutMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    SOCKADDR_BTH sa{};
    sa.addressFamily = AF_BTH;
    sa.btAddr        = static_cast<BTH_ADDR>(address);
    sa.port          = kAACPPsm;

    if (::connect(sock, reinterpret_cast<SOCKADDR*>(&sa), sizeof(sa)) != 0) {
        log::debug("AACP    connect() failed: {}", WSAGetLastError());
        closesocket(sock);
        return false;
    }

    // Send 16-byte opening handshake immediately.
    if (send(sock, reinterpret_cast<const char*>(kHandshake),
             static_cast<int>(sizeof(kHandshake)), 0) <= 0) {
        log::error("AACP    Handshake send failed: {}", WSAGetLastError());
        closesocket(sock);
        return false;
    }

    // Wait for first valid AACP frame: must start with 04 00 04 00.
    uint8_t buf[256];
    int n = recv(sock, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0);
    if (n < 4 || std::memcmp(buf, kAACPHeader, 4) != 0) {
        int err = WSAGetLastError();
        log::debug("AACP    No valid handshake response (n={}, err={})", n, err);
        closesocket(sock);
        return false;
    }

    // Send notification-enable packet to subscribe to stem, battery, ear-detect.
    send(sock, reinterpret_cast<const char*>(kEnableNotifications),
         static_cast<int>(sizeof(kEnableNotifications)), 0);

    // Switch to watchdog timeout for the read loop.
    timeout = kWatchdogMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    m_socket.store(static_cast<uintptr_t>(sock));
    return true;
}

void AACPClient::readLoop() {
    uint8_t buf[512];

    while (m_running.load()) {
        auto rawSock = m_socket.load();
        if (rawSock == kInvalidSocket) break;
        auto sock = static_cast<SOCKET>(rawSock);

        int n = recv(sock, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0);

        if (n <= 0) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                log::warn("AACP    Watchdog: no data for {} s — reconnecting",
                          kWatchdogMs / 1000);
            } else if (err != WSAEINTR && m_running.load()) {
                log::debug("AACP    recv() error: {}", err);
            }
            break;
        }

        // All AACP packets start with the 4-byte header.
        if (n < 5 || std::memcmp(buf, kAACPHeader, 4) != 0) continue;

        uint8_t opcode = buf[kOpcodeOffset];

        if (opcode == kStemPressOpcode && n >= kStemPressMinLen) {
            auto type = static_cast<StemPressType>(buf[6]);
            auto bud  = static_cast<AirPodsBud>(buf[7]);
            const char* budName = (bud == AirPodsBud::Left) ? "left" : "right";
            log::info("AACP    Stem press: type=0x{:02X} bud={}", buf[6], budName);
            if (m_onStemPress) m_onStemPress(type, bud);
        }
    }

    // Close the socket so the next connectAndHandshake starts fresh.
    auto old = m_socket.exchange(kInvalidSocket);
    if (old != kInvalidSocket) closesocket(static_cast<SOCKET>(old));
}

} // namespace Propods
