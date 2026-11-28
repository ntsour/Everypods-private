// le_spike: phase 0 checks for moving the Windows peer links to Bluetooth LE
// (docs/windows-peer-links-le-plan.md, gates G2 and G3). Throwaway; not part of Propods.
//
//   le_spike devices                      paired classic and LE devices Windows knows, with their addresses
//   le_spike scan [--seconds N] [--active]
//                                         phones advertising the EveryPods peer service
//   le_spike connect [--addr AA:BB:..] [--classic AA:BB:..] [--tries N] [--no-hello] [--cached]
//                                         G3: connect, discover the service, subscribe, wait for the phone's HELLO,
//                                         then disconnect; repeated N times per phone, with timings and a summary
//   le_spike hold [--minutes N] [--gatt] [--ping S] [--addr AA:BB:..]
//                                         G2: keep an idle LE link to every advertising phone (or one) and log
//                                         every change, while the AirPods idle test runs. --ping S sends a PING
//                                         every S seconds (the phone answers PONG), so the links carry traffic
//
// Every line also goes to le-spike-<time>.log in the current folder.

#include "v2/PeerV2Identity.hpp"

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Storage::Streams;
using Clock = std::chrono::steady_clock;

namespace {

// docs/peer-ble-transport.md §3
constexpr guid kService{0x78fc4c78, 0x0bdc, 0x435d, {0xb4, 0xa8, 0x7d, 0x8b, 0x5f, 0x0c, 0x0a, 0x04}};
constexpr guid kRx{0x78fc4c78, 0x0bdc, 0x435d, {0xb4, 0xa8, 0x7d, 0x8b, 0x5f, 0x0c, 0x0a, 0x05}};
constexpr guid kTx{0x78fc4c78, 0x0bdc, 0x435d, {0xb4, 0xa8, 0x7d, 0x8b, 0x5f, 0x0c, 0x0a, 0x06}};

constexpr std::uint8_t kHello = 0x01, kLegacy = 0x02, kPing = 0x03, kPong = 0x04, kV2 = 0x05;

// ---- logging ---------------------------------------------------------------

std::mutex g_logMutex;
std::ofstream g_logFile;

std::string now() {
    const auto t = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count() % 1000;
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    localtime_s(&tm, &tt);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return std::format("{}.{:03}", buf, ms);
}

template <class... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
    const std::string line = now() + " " + std::format(fmt, std::forward<Args>(args)...);
    std::lock_guard lock(g_logMutex);
    std::cout << line << std::endl;
    if (g_logFile) g_logFile << line << std::endl;
}

double secondsSince(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// ---- small helpers ---------------------------------------------------------

std::string addr(std::uint64_t a) { return Propods::v2::formatAddress(a); }

std::optional<std::uint64_t> parseAddr(const std::string& s) {
    std::uint64_t v = 0;
    int digits = 0;
    for (char c : s) {
        if (c == ':' || c == '-') continue;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return std::nullopt;
        v = (v << 4) | static_cast<std::uint64_t>(d);
        ++digits;
    }
    if (digits != 12) return std::nullopt;
    return v;
}

const char* str(BluetoothAddressType t) {
    switch (t) {
    case BluetoothAddressType::Public: return "public";
    case BluetoothAddressType::Random: return "random";
    default: return "unspecified";
    }
}

const char* str(GattCommunicationStatus s) {
    switch (s) {
    case GattCommunicationStatus::Success: return "Success";
    case GattCommunicationStatus::Unreachable: return "Unreachable";
    case GattCommunicationStatus::ProtocolError: return "ProtocolError";
    case GattCommunicationStatus::AccessDenied: return "AccessDenied";
    default: return "?";
    }
}

const char* str(BluetoothConnectionStatus s) {
    return s == BluetoothConnectionStatus::Connected ? "Connected" : "Disconnected";
}

const char* str(GattSessionStatus s) { return s == GattSessionStatus::Active ? "Active" : "Closed"; }

std::string protoErr(IReference<std::uint8_t> const& e) {
    return e ? std::format(" (ATT error 0x{:02X})", e.Value()) : std::string();
}

std::string hex(const std::vector<std::uint8_t>& b, std::size_t max = 64) {
    std::string out;
    for (std::size_t i = 0; i < b.size() && i < max; ++i) out += std::format("{:02x}", b[i]);
    if (b.size() > max) out += "...";
    return out;
}

std::string text(const std::vector<std::uint8_t>& b, std::size_t max = 300) {
    std::string s(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(std::min(b.size(), max)));
    if (b.size() > max) s += "...";
    return s;
}

std::vector<std::uint8_t> bytes(IBuffer const& buf) {
    std::vector<std::uint8_t> v(buf.Length());
    if (!v.empty()) DataReader::FromBuffer(buf).ReadBytes(v);
    return v;
}

IBuffer buffer(const std::vector<std::uint8_t>& v) {
    DataWriter w;
    w.WriteBytes(v);
    return w.DetachBuffer();
}

std::vector<std::uint8_t> record(std::uint8_t type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out{type, static_cast<std::uint8_t>(payload.size() >> 8),
                                  static_cast<std::uint8_t>(payload.size() & 0xFF)};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::string computerName() {
    wchar_t buf[256];
    DWORD n = 256;
    if (!GetComputerNameW(buf, &n)) return "Windows";
    return to_string(std::wstring_view(buf, n));
}

void sleepFor(double seconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(seconds * 1000)));
}

// ---- options ---------------------------------------------------------------

struct Options {
    std::string command;
    int seconds = 15;
    bool active = false;
    std::optional<std::uint64_t> addr;     // advertised LE address to pick
    std::optional<std::uint64_t> classic;  // connect by this (classic / identity) address, without scanning
    int tries = 10;
    bool hello = true;                     // connect: send our HELLO (the phone then logs the address it sees)
    bool cached = false;
    double minutes = 10;
    bool gatt = false;                     // hold: also discover and subscribe
    double ping = 0;                       // hold: send a PING record every N s (needs --gatt), so the links carry traffic
    double waitHello = 10;
};

// ---- advertisement scan ----------------------------------------------------

struct Advert {
    std::uint64_t address{};
    BluetoothAddressType type{BluetoothAddressType::Unspecified};
    short rssi{};
    bool connectable{};
    int count{};
};

// Scans for the EveryPods service. Stops early once `want` (if any) was seen.
std::map<std::uint64_t, Advert> scan(double seconds, bool active, std::optional<std::uint64_t> want, bool quiet = false) {
    std::map<std::uint64_t, Advert> found;
    std::mutex m;
    std::condition_variable cv;
    BluetoothLEAdvertisementWatcher w;
    w.ScanningMode(active ? BluetoothLEScanningMode::Active : BluetoothLEScanningMode::Passive);
    w.AdvertisementFilter().Advertisement().ServiceUuids().Append(kService);
    w.Received([&](auto&&, BluetoothLEAdvertisementReceivedEventArgs const& a) {
        std::lock_guard lock(m);
        auto& ad = found[a.BluetoothAddress()];
        const bool first = ad.count == 0;
        ad.address = a.BluetoothAddress();
        ad.type = a.BluetoothAddressType();
        ad.rssi = a.RawSignalStrengthInDBm();
        ad.connectable = a.IsConnectable();
        ++ad.count;
        if (first && !quiet) {
            log("  advert {} ({}) RSSI {} dBm{}", addr(ad.address), str(ad.type), ad.rssi,
                ad.connectable ? "" : " NOT connectable");
        }
        cv.notify_all();
    });
    w.Stopped([&](auto&&, BluetoothLEAdvertisementWatcherStoppedEventArgs const& a) {
        if (a.Error() != BluetoothError::Success) log("  watcher stopped with error {}", static_cast<int>(a.Error()));
    });
    w.Start();
    {
        std::unique_lock lock(m);
        cv.wait_for(lock, std::chrono::milliseconds(static_cast<int>(seconds * 1000)),
                    [&] { return want && found.count(*want) > 0; });
    }
    w.Stop();
    std::lock_guard lock(m);
    return found;
}

// ---- one LE link -----------------------------------------------------------

struct Link {
    std::string label;
    BluetoothLEDevice dev{nullptr};
    GattSession session{nullptr};
    GattDeviceService svc{nullptr};
    GattCharacteristic rx{nullptr};
    GattCharacteristic tx{nullptr};
    event_token connTok{}, sessTok{}, valTok{}, svcTok{}, pduTok{};

    std::mutex m;
    std::condition_variable cv;
    std::vector<std::uint8_t> pending;  // record stream
    bool gotHello = false;
    std::string helloJson;
    std::atomic<bool> servicesChanged{false};
    int connectedEdges = 0, disconnectedEdges = 0;

    std::uint16_t chunkSize() {
        const std::uint16_t pdu = session ? session.MaxPduSize() : 23;
        return static_cast<std::uint16_t>(std::min<int>(pdu > 3 ? pdu - 3 : 20, 512));
    }

    void onBytes(const std::vector<std::uint8_t>& b) {
        std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> records;
        {
            std::lock_guard lock(m);
            pending.insert(pending.end(), b.begin(), b.end());
            std::size_t off = 0;
            while (pending.size() - off >= 3) {
                const std::uint8_t type = pending[off];
                const std::size_t len = (static_cast<std::size_t>(pending[off + 1]) << 8) | pending[off + 2];
                if (type == 0 || len > 4096) {
                    log("[{}] bad stream (type {} length {}); clearing", label, type, len);
                    pending.clear();
                    return;
                }
                if (pending.size() - off - 3 < len) break;
                records.emplace_back(type, std::vector<std::uint8_t>(pending.begin() + static_cast<std::ptrdiff_t>(off + 3),
                                                                     pending.begin() + static_cast<std::ptrdiff_t>(off + 3 + len)));
                off += 3 + len;
            }
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(off));
        }
        for (auto& [type, payload] : records) onRecord(type, payload);
    }

    void onRecord(std::uint8_t type, const std::vector<std::uint8_t>& p) {
        switch (type) {
        case kHello:
            log("[{}] HELLO from phone: {}", label, text(p));
            {
                std::lock_guard lock(m);
                gotHello = true;
                helloJson = text(p);
            }
            cv.notify_all();
            break;
        case kLegacy: log("[{}] LEGACY {}", label, hex(p)); break;
        case kPing:
            log("[{}] PING {} -> PONG", label, hex(p));
            send(kPong, p, false);
            break;
        case kPong: log("[{}] PONG {}", label, hex(p)); break;
        case kV2: log("[{}] V2 {}", label, text(p, 160)); break;
        default: log("[{}] record type 0x{:02x}, {} bytes", label, type, p.size()); break;
        }
    }

    // Sends one record. `wait` = write with response and report the result (else fire and forget).
    bool send(std::uint8_t type, const std::vector<std::uint8_t>& payload, bool wait) {
        if (!rx) return false;
        const auto all = record(type, payload);
        const std::size_t size = chunkSize();
        for (std::size_t off = 0; off < all.size(); off += size) {
            std::vector<std::uint8_t> chunk(all.begin() + static_cast<std::ptrdiff_t>(off),
                                            all.begin() + static_cast<std::ptrdiff_t>(std::min(all.size(), off + size)));
            try {
                if (wait) {
                    auto r = rx.WriteValueWithResultAsync(buffer(chunk), GattWriteOption::WriteWithResponse).get();
                    if (r.Status() != GattCommunicationStatus::Success) {
                        log("[{}] write failed: {}{}", label, str(r.Status()), protoErr(r.ProtocolError()));
                        return false;
                    }
                } else {
                    rx.WriteValueAsync(buffer(chunk), GattWriteOption::WriteWithoutResponse);
                }
            } catch (hresult_error const& e) {
                log("[{}] write threw 0x{:08X} {}", label, static_cast<std::uint32_t>(e.code()), to_string(e.message()));
                return false;
            }
        }
        return true;
    }
};

// Opens the device and a GATT session that keeps the link up. Returns false on failure.
bool openDevice(Link& l, std::uint64_t address, std::optional<BluetoothAddressType> type) {
    const auto t0 = Clock::now();
    try {
        l.dev = type ? BluetoothLEDevice::FromBluetoothAddressAsync(address, *type).get()
                     : BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
    } catch (hresult_error const& e) {
        log("[{}] FromBluetoothAddressAsync threw 0x{:08X} {}", l.label, static_cast<std::uint32_t>(e.code()),
            to_string(e.message()));
        return false;
    }
    if (!l.dev) {
        log("[{}] FromBluetoothAddressAsync returned nothing ({:.2f} s)", l.label, secondsSince(t0));
        return false;
    }
    log("[{}] device '{}' {} ({}), paired={}, status {} ({:.2f} s)", l.label, to_string(l.dev.Name()),
        addr(l.dev.BluetoothAddress()), str(l.dev.BluetoothAddressType()),
        l.dev.DeviceInformation().Pairing().IsPaired(), str(l.dev.ConnectionStatus()), secondsSince(t0));

    Link* lp = &l;
    l.connTok = l.dev.ConnectionStatusChanged([lp](BluetoothLEDevice const& d, auto&&) {
        const auto s = d.ConnectionStatus();
        if (s == BluetoothConnectionStatus::Connected) ++lp->connectedEdges; else ++lp->disconnectedEdges;
        log("[{}] link {}", lp->label, str(s));
    });
    l.svcTok = l.dev.GattServicesChanged([lp](auto&&, auto&&) {
        lp->servicesChanged = true;
        log("[{}] GattServicesChanged", lp->label);
    });

    try {
        l.session = GattSession::FromDeviceIdAsync(l.dev.BluetoothDeviceId()).get();
    } catch (hresult_error const& e) {
        log("[{}] GattSession threw 0x{:08X} {}", l.label, static_cast<std::uint32_t>(e.code()), to_string(e.message()));
        return false;
    }
    if (!l.session) {
        log("[{}] GattSession returned nothing", l.label);
        return false;
    }
    l.sessTok = l.session.SessionStatusChanged([lp](GattSession const&, GattSessionStatusChangedEventArgs const& a) {
        log("[{}] GATT session {} (error {})", lp->label, str(a.Status()), static_cast<int>(a.Error()));
    });
    l.pduTok = l.session.MaxPduSizeChanged([lp](GattSession const& s, auto&&) {
        log("[{}] MaxPduSize {}", lp->label, s.MaxPduSize());
    });
    l.session.MaintainConnection(true);
    log("[{}] session open, MaintainConnection on, CanMaintainConnection={} ({:.2f} s)", l.label,
        l.session.CanMaintainConnection(), secondsSince(t0));
    return true;
}

// Discovers the service and characteristics and subscribes to TX. `retries` on Unreachable.
bool discoverAndSubscribe(Link& l, bool cached, int retries) {
    const auto mode = cached ? BluetoothCacheMode::Cached : BluetoothCacheMode::Uncached;
    const auto t0 = Clock::now();
    GattDeviceServicesResult sr{nullptr};
    for (int i = 1; i <= retries; ++i) {
        const auto ti = Clock::now();
        try {
            sr = l.dev.GetGattServicesForUuidAsync(kService, mode).get();
        } catch (hresult_error const& e) {
            log("[{}] service discovery threw 0x{:08X} {}", l.label, static_cast<std::uint32_t>(e.code()),
                to_string(e.message()));
            sr = nullptr;
        }
        if (sr) {
            log("[{}] service discovery {} try {}: {}{}, {} service(s), {:.2f} s", l.label, cached ? "cached" : "uncached",
                i, str(sr.Status()), protoErr(sr.ProtocolError()), sr.Services().Size(), secondsSince(ti));
            if (sr.Status() == GattCommunicationStatus::Success && sr.Services().Size() > 0) break;
            if (sr.Status() == GattCommunicationStatus::Success) {
                // Not there: list what is, to tell a stale cache from a missing server.
                try {
                    auto all = l.dev.GetGattServicesAsync(mode).get();
                    for (auto const& s : all.Services()) log("[{}]   has service {}", l.label, to_string(to_hstring(s.Uuid())));
                } catch (...) {}
            }
        }
        sr = nullptr;
    }
    if (!sr) {
        log("[{}] FAIL: no EveryPods service after {} tries ({:.2f} s)", l.label, retries, secondsSince(t0));
        return false;
    }
    l.svc = sr.Services().GetAt(0);

    auto chars = [&](guid const& uuid) -> GattCharacteristic {
        auto r = l.svc.GetCharacteristicsForUuidAsync(uuid, mode).get();
        if (r.Status() != GattCommunicationStatus::Success || r.Characteristics().Size() == 0) {
            log("[{}] characteristic {} : {}{}", l.label, to_string(to_hstring(uuid)), str(r.Status()), protoErr(r.ProtocolError()));
            return nullptr;
        }
        return r.Characteristics().GetAt(0);
    };
    l.rx = chars(kRx);
    l.tx = chars(kTx);
    if (!l.rx || !l.tx) {
        log("[{}] FAIL: characteristics missing", l.label);
        return false;
    }
    Link* lp = &l;
    l.valTok = l.tx.ValueChanged([lp](auto&&, GattValueChangedEventArgs const& a) { lp->onBytes(bytes(a.CharacteristicValue())); });
    auto wr = l.tx.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
                      GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
    log("[{}] subscribe (encrypted CCCD): {}{}, MaxPduSize {}, {:.2f} s since discovery start", l.label, str(wr.Status()),
        protoErr(wr.ProtocolError()), l.session.MaxPduSize(), secondsSince(t0));
    return wr.Status() == GattCommunicationStatus::Success;
}

void closeLink(Link& l) {
    try {
        if (l.tx && l.valTok) l.tx.ValueChanged(l.valTok);
        if (l.svc) l.svc.Close();
        if (l.session) {
            if (l.sessTok) l.session.SessionStatusChanged(l.sessTok);
            if (l.pduTok) l.session.MaxPduSizeChanged(l.pduTok);
            l.session.MaintainConnection(false);
            l.session.Close();
        }
        if (l.dev) {
            // Wait for the link to go down, so the next try starts cold (Bleak's teardown order).
            const auto t0 = Clock::now();
            while (l.dev.ConnectionStatus() == BluetoothConnectionStatus::Connected && secondsSince(t0) < 10) sleepFor(0.2);
            log("[{}] closed; link {} after {:.1f} s", l.label, str(l.dev.ConnectionStatus()), secondsSince(t0));
            if (l.connTok) l.dev.ConnectionStatusChanged(l.connTok);
            if (l.svcTok) l.dev.GattServicesChanged(l.svcTok);
            l.dev.Close();
        }
    } catch (hresult_error const& e) {
        log("[{}] close threw 0x{:08X}", l.label, static_cast<std::uint32_t>(e.code()));
    }
    l.svc = nullptr;
    l.rx = l.tx = nullptr;
    l.session = nullptr;
    l.dev = nullptr;
}

std::vector<std::uint8_t> helloPayload() {
    const auto& id = Propods::v2::localIdentity();
    const std::string json = std::format(R"({{"caps":["legacy1"],"id":"{}","name":"{}","v":1}})", id.installationId,
                                         computerName());
    return {json.begin(), json.end()};
}

// ---- commands --------------------------------------------------------------

int cmdDevices() {
    log("Paired classic devices:");
    for (auto const& d : DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true)).get()) {
        try {
            auto bd = BluetoothDevice::FromIdAsync(d.Id()).get();
            if (bd) log("  {} '{}' {}", addr(bd.BluetoothAddress()), to_string(bd.Name()),
                        bd.ConnectionStatus() == BluetoothConnectionStatus::Connected ? "connected" : "");
        } catch (...) { log("  '{}' (unreadable)", to_string(d.Name())); }
    }
    log("Paired LE devices (Windows has LE keys for these):");
    for (auto const& d : DeviceInformation::FindAllAsync(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)).get()) {
        try {
            auto ld = BluetoothLEDevice::FromIdAsync(d.Id()).get();
            if (ld) log("  {} ({}) '{}' {} protection {}", addr(ld.BluetoothAddress()), str(ld.BluetoothAddressType()),
                        to_string(ld.Name()), str(ld.ConnectionStatus()),
                        static_cast<int>(ld.DeviceInformation().Pairing().ProtectionLevel()));
        } catch (...) { log("  '{}' (unreadable)", to_string(d.Name())); }
    }
    return 0;
}

int cmdScan(const Options& o) {
    log("Scanning {} s ({}) for the EveryPods peer service...", o.seconds, o.active ? "active" : "passive");
    auto found = scan(o.seconds, o.active, std::nullopt);
    log("{} advertiser(s):", found.size());
    for (auto const& [a, ad] : found) {
        std::string name;
        try {
            if (auto d = BluetoothLEDevice::FromBluetoothAddressAsync(a, ad.type).get()) name = to_string(d.Name());
        } catch (...) {}
        log("  {} ({}) '{}' RSSI {} dBm, {} adverts", addr(a), str(ad.type), name, ad.rssi, ad.count);
    }
    return 0;
}

struct Target {
    std::uint64_t address;
    std::optional<BluetoothAddressType> type;
    std::string label;
};

// Advertising phones (or the one asked for). The label is the phone's name when Windows knows it.
std::vector<Target> findTargets(const Options& o) {
    if (o.classic) return {{*o.classic, std::nullopt, addr(*o.classic) + " classic"}};
    log("Scanning up to {} s for the EveryPods peer service...", o.seconds);
    auto found = scan(o.seconds, o.active, o.addr);
    std::vector<Target> out;
    for (auto const& [a, ad] : found) {
        if (o.addr && a != *o.addr) continue;
        std::string name = addr(a);
        try {
            if (auto d = BluetoothLEDevice::FromBluetoothAddressAsync(a, ad.type).get(); d && !d.Name().empty())
                name = to_string(d.Name());
        } catch (...) {}
        out.push_back({a, ad.type, name});
    }
    if (out.empty()) log("No phone advertising the EveryPods service. Is EveryPods open on the phone with this PC configured?");
    return out;
}

int cmdConnect(const Options& o) {
    auto targets = findTargets(o);
    if (targets.empty()) return 1;
    struct Result { int ok = 0; std::vector<double> times; };
    std::map<std::string, Result> results;
    for (int t = 1; t <= o.tries; ++t) {
        for (auto& target : targets) {
            log("=== {} try {}/{} ===", target.label, t, o.tries);
            if (t > 1 && !o.classic && target.type == BluetoothAddressType::Random) {
                // A private address may have rotated: find the phone again (stops as soon as it is seen).
                auto again = scan(5, false, target.address, true);
                if (!again.count(target.address)) log("[{}] its address {} is no longer advertised", target.label, addr(target.address));
            }
            Link l;
            l.label = target.label;
            const auto t0 = Clock::now();
            bool ok = openDevice(l, target.address, target.type) && discoverAndSubscribe(l, o.cached, 3);
            if (ok) {
                std::unique_lock lock(l.m);
                ok = l.cv.wait_for(lock, std::chrono::milliseconds(static_cast<int>(o.waitHello * 1000)), [&] { return l.gotHello; });
            }
            const double took = secondsSince(t0);
            if (ok) {
                log("[{}] PASS: HELLO {:.2f} s after start", l.label, took);
                results[target.label].ok++;
                results[target.label].times.push_back(took);
                if (o.hello) {
                    const bool sent = l.send(kHello, helloPayload(), true);
                    log("[{}] our HELLO (encrypted write) {}", l.label, sent ? "accepted" : "FAILED");
                    sleepFor(2);  // let the phone answer; it logs the address it sees for this PC
                }
            } else {
                log("[{}] FAIL after {:.2f} s", l.label, took);
                results[target.label];
            }
            closeLink(l);
            sleepFor(3);
        }
    }
    log("=== Summary ===");
    for (auto& [label, r] : results) {
        std::sort(r.times.begin(), r.times.end());
        const double median = r.times.empty() ? 0 : r.times[r.times.size() / 2];
        const double worst = r.times.empty() ? 0 : r.times.back();
        log("{}: {}/{} passed, median {:.2f} s, worst {:.2f} s", label, r.ok, o.tries, median, worst);
    }
    return 0;
}

int cmdHold(const Options& o) {
    auto targets = findTargets(o);
    if (targets.empty()) return 1;
    std::vector<std::unique_ptr<Link>> links;
    for (auto& target : targets) {
        auto l = std::make_unique<Link>();
        l->label = target.label;
        if (!openDevice(*l, target.address, target.type)) continue;
        if (o.gatt) discoverAndSubscribe(*l, o.cached, 3);
        links.push_back(std::move(l));
    }
    if (links.empty()) return 1;
    // MaintainConnection only asks Windows to connect; wait for the links to come up.
    const auto t0 = Clock::now();
    while (secondsSince(t0) < 30) {
        bool all = true;
        for (auto& l : links) all = all && l->dev.ConnectionStatus() == BluetoothConnectionStatus::Connected;
        if (all) break;
        sleepFor(0.5);
    }
    for (auto& l : links) log("[{}] link {} after {:.1f} s", l->label, str(l->dev.ConnectionStatus()), secondsSince(t0));
    log("Holding {} link(s) for {:.0f} min. Run the AirPods idle test now. Ctrl+C stops early.", links.size(), o.minutes);
    const auto start = Clock::now();
    auto nextStatus = Clock::now() + std::chrono::minutes(1);
    auto nextPing = Clock::now();
    std::uint32_t seq = 0;
    while (secondsSince(start) < o.minutes * 60) {
        sleepFor(o.ping > 0 && o.ping < 1 ? o.ping : 1);
        if (o.ping > 0 && Clock::now() >= nextPing) {
            nextPing = Clock::now() + std::chrono::milliseconds(static_cast<int>(o.ping * 1000));
            ++seq;
            const std::vector<std::uint8_t> p{static_cast<std::uint8_t>(seq >> 24), static_cast<std::uint8_t>(seq >> 16),
                                              static_cast<std::uint8_t>(seq >> 8), static_cast<std::uint8_t>(seq)};
            for (auto& l : links)
                if (l->rx && !l->send(kPing, p, false)) log("[{}] PING {} not sent", l->label, seq);
        }
        for (auto& l : links) {
            if (o.gatt && l->servicesChanged.exchange(false)) {
                log("[{}] rediscovering after GattServicesChanged", l->label);
                const auto tr = Clock::now();
                {
                    std::lock_guard lock(l->m);
                    l->gotHello = false;
                    l->pending.clear();
                }
                if (l->tx && l->valTok) l->tx.ValueChanged(l->valTok);
                l->valTok = {};
                if (l->svc) l->svc.Close();
                l->svc = nullptr;
                l->rx = l->tx = nullptr;
                bool ok = discoverAndSubscribe(*l, false, 3);
                if (ok) {
                    std::unique_lock lock(l->m);
                    ok = l->cv.wait_for(lock, std::chrono::seconds(10), [&] { return l->gotHello; });
                }
                log("[{}] rediscovery {} after {:.2f} s", l->label, ok ? "PASS" : "FAIL", secondsSince(tr));
            }
        }
        if (Clock::now() >= nextStatus) {
            nextStatus += std::chrono::minutes(1);
            for (auto& l : links)
                log("[{}] status: link {}, session {}, ups {}, downs {}", l->label, str(l->dev.ConnectionStatus()),
                    str(l->session.SessionStatus()), l->connectedEdges, l->disconnectedEdges);
        }
    }
    for (auto& l : links) closeLink(*l);
    return 0;
}

void usage() {
    std::cout <<
        "le_spike devices\n"
        "le_spike scan    [--seconds N] [--active]\n"
        "le_spike connect [--addr AA:BB:CC:DD:EE:FF] [--classic AA:BB:CC:DD:EE:FF] [--tries N] [--no-hello] [--cached] [--seconds N]\n"
        "le_spike hold    [--minutes N] [--gatt] [--ping SECONDS] [--addr AA:BB:CC:DD:EE:FF] [--seconds N]\n";
}

}  // namespace

int main(int argc, char** argv) {
    init_apartment(apartment_type::multi_threaded);
    if (argc < 2) { usage(); return 2; }
    Options o;
    o.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--seconds") o.seconds = std::stoi(next());
        else if (a == "--active") o.active = true;
        else if (a == "--addr") o.addr = parseAddr(next());
        else if (a == "--classic") o.classic = parseAddr(next());
        else if (a == "--tries") o.tries = std::stoi(next());
        else if (a == "--no-hello") o.hello = false;
        else if (a == "--cached") o.cached = true;
        else if (a == "--minutes") o.minutes = std::stod(next());
        else if (a == "--gatt") o.gatt = true;
        else if (a == "--ping") { o.ping = std::stod(next()); o.gatt = true; }
        else { std::cout << "unknown option " << a << "\n"; usage(); return 2; }
    }
    const auto stamp = now().substr(0, 19);
    std::string file = "le-spike-" + stamp + ".log";
    std::replace(file.begin(), file.end(), ':', '-');
    std::replace(file.begin(), file.end(), ' ', '_');
    g_logFile.open(file);
    log("le_spike {} (installation {}), log {}", o.command, Propods::v2::localIdentity().installationId, file);
    try {
        if (o.command == "devices") return cmdDevices();
        if (o.command == "scan") return cmdScan(o);
        if (o.command == "connect") return cmdConnect(o);
        if (o.command == "hold") return cmdHold(o);
    } catch (hresult_error const& e) {
        log("error 0x{:08X} {}", static_cast<std::uint32_t>(e.code()), to_string(e.message()));
        return 1;
    }
    usage();
    return 2;
}
