#include "BtHandoverSetup.hpp"

#include "BluetoothRfcommClient.hpp"
#include "Logger.hpp"

#include <objbase.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace Propods {

namespace {

using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage::Streams;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

// Must match BtHandoverSync.SERVICE_UUID on Android.
constexpr winrt::guid kSetupService{
    0xde51a5b3, 0x4140, 0x4fb7, {0xa5, 0x24, 0x96, 0x73, 0x79, 0x18, 0x17, 0x73}};
constexpr std::uint32_t kMaxMessageBytes = 1024;
constexpr auto kExchangeTimeout = std::chrono::seconds{10};

std::filesystem::path statePath() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::filesystem::path dir = (len > 0 && len < MAX_PATH)
        ? std::filesystem::path{buf} / L"Propods"
        : std::filesystem::current_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / L"bt-handover-sync.txt";
}

bool isCanonicalUuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// Same ordering as Android HandoverSyncState.compareTo: revision, then a
// removal beats a creation, then origin.
bool greater(const BtHandoverSetupServer::State& a, const BtHandoverSetupServer::State& b) {
    if (a.revision != b.revision) return a.revision > b.revision;
    if (a.enabled != b.enabled) return !a.enabled;
    return a.origin > b.origin;
}

BtHandoverSetupServer::State decode(const std::string& text) {
    const auto json = JsonObject::Parse(winrt::to_hstring(text));
    if (json.Size() != 4) throw std::runtime_error("unexpected fields");
    const auto version = json.GetNamedValue(L"version");
    const auto revision = json.GetNamedValue(L"revision");
    const auto origin = json.GetNamedValue(L"origin");
    const auto enabled = json.GetNamedValue(L"enabled");
    if (version.ValueType() != JsonValueType::Number || version.GetNumber() != 1.0)
        throw std::runtime_error("unsupported version");
    if (revision.ValueType() != JsonValueType::Number) throw std::runtime_error("bad revision");
    const double rev = revision.GetNumber();
    if (rev < 1 || rev > 9.0e15 || rev != static_cast<double>(static_cast<std::int64_t>(rev)))
        throw std::runtime_error("bad revision");
    if (origin.ValueType() != JsonValueType::String || enabled.ValueType() != JsonValueType::Boolean)
        throw std::runtime_error("bad origin/enabled");
    BtHandoverSetupServer::State s;
    s.revision = static_cast<std::int64_t>(rev);
    s.origin = winrt::to_string(origin.GetString());
    s.enabled = enabled.GetBoolean();
    if (!isCanonicalUuid(s.origin)) throw std::runtime_error("bad origin");
    return s;
}

std::string encode(const BtHandoverSetupServer::State& s) {
    // Written by hand so "version" is the integer 1, as Android requires.
    return std::format(R"({{"version":1,"revision":{},"origin":"{}","enabled":{}}})",
        s.revision, s.origin, s.enabled ? "true" : "false");
}

std::string newOrigin() {
    GUID g{};
    if (FAILED(CoCreateGuid(&g))) throw std::runtime_error("CoCreateGuid failed");
    return std::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

// One request and one reply on a fresh connection to the phone's setup
// service, the phone's side of BtHandoverSync (u32 big-endian length + JSON).
BtHandoverSetupServer::State exchangeWithPhone(std::uint64_t phone, const BtHandoverSetupServer::State& request) {
    StreamSocket socket = dialRfcomm(phone, kSetupService, "SETUP");
    std::atomic<bool> done{false};
    std::thread watchdog([&done, socket]() {
        const auto deadline = std::chrono::steady_clock::now() + kExchangeTimeout;
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!done.load()) { try { socket.Close(); } catch (...) {} }
    });
    const auto finish = [&] {
        done.store(true);
        watchdog.join();
        try { socket.Close(); } catch (...) {}
    };
    try {
        DataWriter writer{socket.OutputStream()};  // big-endian by default
        DataReader reader{socket.InputStream()};
        const std::string body = encode(request);
        writer.WriteUInt32(static_cast<std::uint32_t>(body.size()));
        writer.WriteBytes(winrt::array_view<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(body.data()),
            reinterpret_cast<const std::uint8_t*>(body.data()) + body.size()));
        writer.StoreAsync().get();
        log::info("BT handover setup request to {:012X}: {}", phone, body);
        if (reader.LoadAsync(4).get() != 4)
            throw std::runtime_error("no reply (has this PC been added on that device?)");
        const std::uint32_t length = reader.ReadUInt32();
        if (length < 1 || length > kMaxMessageBytes) throw std::runtime_error("bad reply length");
        if (reader.LoadAsync(length).get() != length) throw std::runtime_error("short reply");
        const std::string reply = winrt::to_string(reader.ReadString(length));
        log::info("BT handover setup reply from {:012X}: {}", phone, reply);
        auto state = decode(reply);
        finish();
        return state;
    } catch (...) {
        finish();
        throw;
    }
}

}  // namespace

BtHandoverSetupServer::BtHandoverSetupServer() { load(); }

BtHandoverSetupServer::~BtHandoverSetupServer() {
    stop();
    std::scoped_lock lk{m_authMutex};
    if (m_authThread.joinable()) m_authThread.join();
}

void BtHandoverSetupServer::authorize(std::uint64_t phoneAddress) {
    std::scoped_lock lk{m_authMutex};
    if (m_authBusy.load()) return;
    const auto now = std::chrono::steady_clock::now();
    auto it = m_lastAuthorize.find(phoneAddress);
    if (it != m_lastAuthorize.end() && now - it->second < kAuthorizeInterval) return;
    m_lastAuthorize[phoneAddress] = now;
    if (m_authThread.joinable()) m_authThread.join();  // the last run has finished
    m_authBusy.store(true);
    m_authThread = std::thread([this, phoneAddress] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        runAuthorize(phoneAddress);
        m_authBusy.store(false);
    });
}

void BtHandoverSetupServer::runAuthorize(std::uint64_t phone) {
    log::handover("SETUP   {:012X} refuses our links: asking it to finish adding this PC", phone);
    try {
        // Revision 1 loses the phone's merge, so it answers with its own state;
        // echoing that back clears the phone's pending setup (the Mac's
        // GattSetupAuthorizer does the same over Bluetooth LE).
        const State first{1, newOrigin(), true};
        const State phoneState = exchangeWithPhone(phone, first);
        std::this_thread::sleep_for(std::chrono::seconds{1});
        const State confirmed = exchangeWithPhone(phone, phoneState);
        const State winner = merge(phone, confirmed);
        log::handover("SETUP   {:012X} authorized this PC (revision {}, {})", phone, winner.revision,
            winner.enabled ? "enabled" : "removed");
    } catch (const winrt::hresult_error& e) {
        log::warn("BT handover setup with {:012X} not finished: 0x{:08X} {}", phone,
            static_cast<std::uint32_t>(e.code().value), winrt::to_string(e.message()));
    } catch (const std::exception& e) {
        log::warn("BT handover setup with {:012X} not finished: {}", phone, e.what());
    }
}

bool BtHandoverSetupServer::start() {
    if (m_running.exchange(true)) return true;
    try {
        m_provider = RfcommServiceProvider::CreateAsync(RfcommServiceId::FromUuid(kSetupService)).get();
        m_listener = StreamSocketListener{};
        m_listener.ConnectionReceived([this](StreamSocketListener const&,
                                             StreamSocketListenerConnectionReceivedEventArgs const& args) {
            auto socket = args.Socket();
            std::thread([this, socket]() {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                handle(socket);
            }).detach();
        });
        m_listener.BindServiceNameAsync(m_provider.ServiceId().AsString(),
            winrt::Windows::Networking::Sockets::SocketProtectionLevel::BluetoothEncryptionWithAuthentication).get();
        m_provider.StartAdvertising(m_listener);
        log::info("BT handover setup service advertised (phones can now add this PC)");
        return true;
    } catch (const winrt::hresult_error& e) {
        log::warn("BT handover setup service failed to start: 0x{:08X} {}",
            static_cast<std::uint32_t>(e.code().value), winrt::to_string(e.message()));
        m_running.store(false);
        m_listener = nullptr;
        m_provider = nullptr;
        return false;
    }
}

void BtHandoverSetupServer::stop() {
    if (!m_running.exchange(false)) return;
    try { if (m_provider) m_provider.StopAdvertising(); } catch (...) {}
    try { if (m_listener) m_listener.Close(); } catch (...) {}
    m_provider = nullptr;
    m_listener = nullptr;
}

void BtHandoverSetupServer::handle(StreamSocket socket) {
    std::uint64_t address = 0;
    try {
        const auto raw = winrt::to_string(socket.Information().RemoteHostName().RawName());
        // RawName is "(AA:BB:CC:DD:EE:FF)"; keep only the hex digits.
        std::string hex;
        for (char c : raw) if (std::isxdigit(static_cast<unsigned char>(c))) hex += c;
        if (hex.size() == 12) address = std::strtoull(hex.c_str(), nullptr, 16);
    } catch (...) {}
    if (address == 0) {
        log::warn("BT handover setup: cannot read the phone's address; closing");
        try { socket.Close(); } catch (...) {}
        return;
    }

    std::atomic<bool> done{false};
    std::thread watchdog([&done, socket]() {
        const auto deadline = std::chrono::steady_clock::now() + kExchangeTimeout;
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!done.load()) { try { socket.Close(); } catch (...) {} }
    });

    try {
        DataReader reader{socket.InputStream()};
        DataWriter writer{socket.OutputStream()};  // big-endian by default
        if (reader.LoadAsync(4).get() != 4) throw std::runtime_error("no request");
        const std::uint32_t length = reader.ReadUInt32();
        if (length < 1 || length > kMaxMessageBytes) throw std::runtime_error("bad length");
        if (reader.LoadAsync(length).get() != length) throw std::runtime_error("short request");
        const std::string request = winrt::to_string(reader.ReadString(length));
        log::info("BT handover setup from {:012X}: {}", address, request);

        const State winner = merge(address, decode(request));
        const std::string reply = encode(winner);
        writer.WriteUInt32(static_cast<std::uint32_t>(reply.size()));
        writer.WriteBytes(winrt::array_view<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(reply.data()),
            reinterpret_cast<const std::uint8_t*>(reply.data()) + reply.size()));
        writer.StoreAsync().get();
        log::info("BT handover setup reply to {:012X}: {}", address, reply);
        // Give the phone a moment to read before the socket closes.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } catch (const winrt::hresult_error& e) {
        log::warn("BT handover setup with {:012X} failed: 0x{:08X} {}", address,
            static_cast<std::uint32_t>(e.code().value), winrt::to_string(e.message()));
    } catch (const std::exception& e) {
        log::warn("BT handover setup with {:012X} failed: {}", address, e.what());
    }
    done.store(true);
    watchdog.join();
    try { socket.Close(); } catch (...) {}
}

BtHandoverSetupServer::State BtHandoverSetupServer::merge(std::uint64_t address, const State& remote) {
    State winner;
    bool changed = false;
    {
        std::scoped_lock lk{m_mutex};
        auto it = m_states.find(address);
        if (it != m_states.end() && !greater(remote, it->second)) {
            winner = it->second;  // ours is the same or newer: reply with it
        } else {
            m_states[address] = remote;
            winner = remote;
            changed = true;
            save();  // commit before replying
        }
    }
    if (changed) {
        log::handover("SETUP   phone {:012X} {} BT handover (revision {})", address,
            winner.enabled ? "enabled" : "removed", winner.revision);
        if (m_onChange) m_onChange(address, winner.enabled);
    }
    return winner;
}

void BtHandoverSetupServer::load() {
    std::ifstream in{statePath()};
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields{line};
        std::string addr, origin;
        std::int64_t revision = 0;
        int enabled = 0;
        if (!(fields >> addr >> revision >> origin >> enabled)) continue;
        m_states[std::strtoull(addr.c_str(), nullptr, 16)] = State{revision, origin, enabled != 0};
    }
}

void BtHandoverSetupServer::save() {
    const auto path = statePath();
    const auto tmp = std::filesystem::path{path}.concat(L".tmp");
    {
        std::ofstream out{tmp, std::ios::trunc};
        for (const auto& [addr, s] : m_states)
            out << std::format("{:012X} {} {} {}\n", addr, s.revision, s.origin, s.enabled ? 1 : 0);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) log::warn("BT handover setup: could not save {}: {}", log::toUtf8(path.wstring()), ec.message());
}

}
