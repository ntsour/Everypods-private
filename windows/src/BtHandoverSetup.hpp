#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Networking.Sockets.h>

namespace Propods {

// Answers the EveryPods Android app's "BT handover setup" exchange
// (RFCOMM service de51a5b3-4140-4fb7-a524-967379181773, see Android
// relay/BtHandoverSync.kt and docs/bt-handover-sync.md).
//
// When the user adds this PC as a BT handover device on the phone, the phone
// marks it pending and dials this service until it gets an answer. Until then
// the phone refuses the PC's CrossDevice connection ("Rejecting unconfigured
// peer"). Each connection carries one request and one reply:
//   u32 big-endian length (1..1024) + UTF-8 JSON with exactly
//   {"version":1,"revision":<int>,"origin":"<uuid>","enabled":<bool>}
// The PC keeps the winning state per phone (higher revision, then removal over
// creation, then larger origin), commits it, and replies with it. Adding the
// PC happens on the phone.
//
// Since 2026-11-22 the Android app no longer dials computers for this: it
// waits for the computer to start the exchange (the Mac does, over Bluetooth
// LE). authorize() is that side on Windows: it dials the phone's setup service
// with the same message format and runs the Mac's two exchanges.
class BtHandoverSetupServer {
public:
    struct State {
        std::int64_t revision = 0;
        std::string origin;
        bool enabled = false;
    };
    // Called after a phone's setup state is committed (enabled or removed).
    using ChangeCallback = std::function<void(std::uint64_t phoneAddress, bool enabled)>;

    BtHandoverSetupServer();
    ~BtHandoverSetupServer();

    void setOnChange(ChangeCallback cb) { m_onChange = std::move(cb); }
    bool start();
    void stop();

    // Ask the phone to finish adding this PC: send {"revision":1,...,"enabled":
    // true}, which loses the phone's merge and is answered with the phone's
    // state, then echo that state back so the phone clears its pending setup
    // and accepts our links. Runs on its own thread; at most one at a time and
    // once per kAuthorizeInterval per phone. A phone where the user has not
    // added this PC refuses (no reply), so calling this for any phone is safe.
    void authorize(std::uint64_t phoneAddress);

private:
    void handle(winrt::Windows::Networking::Sockets::StreamSocket socket);
    State merge(std::uint64_t address, const State& remote);
    void runAuthorize(std::uint64_t phoneAddress);
    void load();
    void save();

    std::mutex m_mutex;
    std::map<std::uint64_t, State> m_states;
    ChangeCallback m_onChange;
    std::atomic<bool> m_running{false};

    static constexpr auto kAuthorizeInterval = std::chrono::minutes{2};
    std::mutex m_authMutex;  // guards m_authThread / m_lastAuthorize
    std::thread m_authThread;
    std::atomic<bool> m_authBusy{false};
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> m_lastAuthorize;
    winrt::Windows::Devices::Bluetooth::Rfcomm::RfcommServiceProvider m_provider{nullptr};
    winrt::Windows::Networking::Sockets::StreamSocketListener m_listener{nullptr};
};

}
