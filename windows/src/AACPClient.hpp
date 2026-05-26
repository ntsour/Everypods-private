#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace librepods {

enum class StemPressType : uint8_t {
    Single = 0x05,
    Double = 0x06,
    Triple = 0x07,
    Long   = 0x08,
    Unknown = 0x00,
};

enum class AirPodsBud : uint8_t {
    Left  = 0x01,
    Right = 0x02,
    Unknown = 0x00,
};

// Connects to AirPods over Bluetooth L2CAP (PSM 0x1001) and receives AACP
// packets. Fires a callback on opcode 0x19 (stem press). Automatically
// reconnects with exponential backoff on connection loss.
class AACPClient {
public:
    using StemPressCallback = std::function<void(StemPressType, AirPodsBud)>;

    AACPClient();
    ~AACPClient();

    AACPClient(const AACPClient&) = delete;
    AACPClient& operator=(const AACPClient&) = delete;

    void setOnStemPress(StemPressCallback cb) { m_onStemPress = std::move(cb); }

    // Start the connection loop in a background thread.
    // Safe to call again after stop() to reconnect to a new address.
    void start(std::uint64_t airpodsAddress);

    // Signal stop and close the socket. Non-blocking; the worker thread exits
    // asynchronously. A subsequent start() will join it first.
    void stop();

    bool isConnected() const { return m_connected.load(); }

private:
    static constexpr uintptr_t kInvalidSocket = ~uintptr_t{0};

    void runLoop(std::uint64_t address);
    bool connectAndHandshake(std::uint64_t address);
    void readLoop();

    std::atomic<bool>      m_running{false};
    std::atomic<bool>      m_connected{false};
    std::atomic<uintptr_t> m_socket{kInvalidSocket};
    std::thread            m_worker;

    StemPressCallback      m_onStemPress;
};

} // namespace librepods
