#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

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
// creation, then larger origin), commits it, and replies with it. The PC never
// starts changes itself; adding the PC happens on the phone.
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

private:
    void handle(winrt::Windows::Networking::Sockets::StreamSocket socket);
    State merge(std::uint64_t address, const State& remote);
    void load();
    void save();

    std::mutex m_mutex;
    std::map<std::uint64_t, State> m_states;
    ChangeCallback m_onChange;
    std::atomic<bool> m_running{false};
    winrt::Windows::Devices::Bluetooth::Rfcomm::RfcommServiceProvider m_provider{nullptr};
    winrt::Windows::Networking::Sockets::StreamSocketListener m_listener{nullptr};
};

}
