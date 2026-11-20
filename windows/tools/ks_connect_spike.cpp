// ks_connect_spike.cpp - STANDALONE spike (not part of the Propods exe).
//
// Goal: test the driver-level Bluetooth audio connect that Windows Settings'
// "Connect" button uses: KSPROPSETID_BtAudio with KSPROPERTY_ONESHOT_RECONNECT /
// KSPROPERTY_ONESHOT_DISCONNECT, sent via IKsControl obtained through Core Audio
// device topology (same approach as ToothTray, github.com/m2jean/ToothTray).
// The property is a GET with no value; S_OK only means "attempt made". Confirmation
// is the endpoint becoming DEVICE_STATE_ACTIVE (connect) or leaving it (disconnect).
//
// Build (VS dev prompt):  windows\tools\build_ks_connect_spike.bat
//   or:  cl /nologo /std:c++20 /EHsc /W4 /permissive- /Zc:__cplusplus /DUNICODE /D_UNICODE
//        /DWIN32_LEAN_AND_MEAN /DNOMINMAX ks_connect_spike.cpp /Fe:ks_connect_spike.exe
//        /link Ole32.lib windowsapp.lib
//   or: CMake target `ks_connect_spike`.
//
// Usage:
//   ks_connect_spike list
//   ks_connect_spike connect    <name-substring>
//   ks_connect_spike disconnect <name-substring>
//   ks_connect_spike cycle      <name-substring> <count> [pauseSeconds=5]
//   ks_connect_spike watch      <name-substring> <minutes>
// Output goes to stdout and is appended to ks_connect_spike.log in the cwd.

#include <initguid.h>  // single TU: makes PKEY_* / KS GUIDs real definitions (selectany)
#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <devicetopology.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cwctype>
#include <functional>
#include <cstdio>
#include <ctime>
#include <exception>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace wbt = winrt::Windows::Devices::Bluetooth;
namespace wde = winrt::Windows::Devices::Enumeration;

namespace {

// KSPROPSETID_BtAudio {7FA06C40-B8F6-4C7E-8556-E8C33A12E54D}, defined locally under a
// different name so we cannot collide with (or miss) a header/lib definition.
constexpr GUID kBtAudioSet = {0x7fa06c40, 0xb8f6, 0x4c7e, {0x85, 0x56, 0xe8, 0xc3, 0x3a, 0x12, 0xe5, 0x4d}};
constexpr ULONG kBtReconnect = 0;   // KSPROPERTY_ONESHOT_RECONNECT
constexpr ULONG kBtDisconnect = 1;  // KSPROPERTY_ONESHOT_DISCONNECT

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- logging
std::mutex g_logMu;
std::ofstream g_logFile;

std::string wallStamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    return std::format("{:02}:{:02}:{:02}.{:03}", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ms));
}

void logLine(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_logMu);
    std::string line = "[" + wallStamp() + "] " + msg;
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (g_logFile.is_open()) {
        g_logFile << line << '\n';
        g_logFile.flush();
    }
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring lowerW(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::string lowerA(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string hrs(HRESULT hr) { return std::format("0x{:08X}", static_cast<unsigned>(hr)); }

std::string stateName(DWORD s) {
    switch (s) {
        case DEVICE_STATE_ACTIVE: return "ACTIVE";
        case DEVICE_STATE_DISABLED: return "DISABLED";
        case DEVICE_STATE_NOTPRESENT: return "NOTPRESENT";
        case DEVICE_STATE_UNPLUGGED: return "UNPLUGGED";
        default: return std::format("0x{:X}", static_cast<unsigned>(s));
    }
}

// ---------------------------------------------------------------- endpoints / KS
struct Endpoint {
    std::wstring id;
    std::wstring name;
    std::wstring filterKey;  // id of the KS filter device the endpoint is wired to
    DWORD state = 0;
    bool render = true;
    ComPtr<IKsControl> ks;
    std::string ksNote;
};

ComPtr<IMMDeviceEnumerator> makeEnumerator(HRESULT* outHr = nullptr) {
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (outHr) *outHr = hr;
    if (FAILED(hr)) en.Reset();
    return en;
}

std::wstring friendlyName(IMMDevice* dev) {
    ComPtr<IPropertyStore> store;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &store))) return L"<unknown>";
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring out = L"<unknown>";
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        out = pv.pwszVal;
    }
    PropVariantClear(&pv);
    return out;
}

// Walk: endpoint -> IDeviceTopology -> connector 0 -> connected-to connector -> IPart -> IKsControl.
HRESULT acquireKs(IMMDevice* dev, ComPtr<IKsControl>& ks, std::wstring& key, std::string& note) {
    ComPtr<IDeviceTopology> topo;
    HRESULT hr = dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(topo.GetAddressOf()));
    if (FAILED(hr)) { note = "Activate(IDeviceTopology) failed " + hrs(hr); return hr; }

    ComPtr<IConnector> conn;
    hr = topo->GetConnector(0, &conn);
    if (FAILED(hr)) { note = "GetConnector(0) failed " + hrs(hr); return hr; }

    LPWSTR connId = nullptr;
    if (SUCCEEDED(conn->GetDeviceIdConnectedTo(&connId)) && connId) {
        key = connId;
        CoTaskMemFree(connId);
    }

    ComPtr<IConnector> other;
    hr = conn->GetConnectedTo(&other);
    if (FAILED(hr)) { note = "GetConnectedTo failed " + hrs(hr); return hr; }

    ComPtr<IPart> part;
    hr = other.As(&part);
    if (FAILED(hr)) { note = "QI(IPart) failed " + hrs(hr); return hr; }

    hr = part->Activate(CLSCTX_ALL, __uuidof(IKsControl), reinterpret_cast<void**>(ks.GetAddressOf()));
    if (FAILED(hr)) { note = "IPart::Activate(IKsControl) failed " + hrs(hr); return hr; }
    return S_OK;
}

HRESULT ksCall(IKsControl* ks, ULONG id, ULONG flags) {
    KSPROPERTY prop{};
    prop.Set = kBtAudioSet;
    prop.Id = id;
    prop.Flags = flags;
    ULONG bytes = 0;
    return ks->KsProperty(&prop, sizeof(prop), nullptr, 0, &bytes);
}

std::string probeSupport(IKsControl* ks) {
    HRESULT hr = ksCall(ks, kBtReconnect, KSPROPERTY_TYPE_SETSUPPORT);
    const char* verdict = "unclear";
    if (hr == S_OK || hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA)) verdict = "yes";
    else if (hr == HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) verdict = "no";
    return std::format("BtAudio set supported: {} (SETSUPPORT hr={})", verdict, hrs(hr));
}

// Enumerates all render+capture endpoints in every state. If needleLower is non-empty only
// matching endpoints are kept. KS is acquired only when wantKs.
bool enumerate(std::vector<Endpoint>& out, const std::wstring& needleLower, bool wantKs) {
    HRESULT hr = S_OK;
    ComPtr<IMMDeviceEnumerator> en = makeEnumerator(&hr);
    if (!en) { logLine("CoCreateInstance(MMDeviceEnumerator) failed " + hrs(hr)); return false; }
    ComPtr<IMMDeviceCollection> col;
    hr = en->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, &col);
    if (FAILED(hr)) { logLine("EnumAudioEndpoints failed " + hrs(hr)); return false; }
    UINT count = 0;
    hr = col->GetCount(&count);
    if (FAILED(hr)) { logLine("GetCount failed " + hrs(hr)); return false; }

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;
        Endpoint ep;
        ep.name = friendlyName(dev.Get());
        if (!needleLower.empty() && lowerW(ep.name).find(needleLower) == std::wstring::npos) continue;

        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) { ep.id = id; CoTaskMemFree(id); }
        DWORD st = 0;
        if (SUCCEEDED(dev->GetState(&st))) ep.state = st;
        ComPtr<IMMEndpoint> mep;
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&mep)))) {
            EDataFlow flow = eRender;
            if (SUCCEEDED(mep->GetDataFlow(&flow))) ep.render = (flow == eRender);
        }
        if (wantKs) {
            HRESULT khr = acquireKs(dev.Get(), ep.ks, ep.filterKey, ep.ksNote);
            if (SUCCEEDED(khr) && ep.ks) ep.ksNote = probeSupport(ep.ks.Get());
            if (ep.filterKey.empty()) ep.filterKey = ep.id;
        }
        out.push_back(std::move(ep));
    }
    return true;
}

// ---------------------------------------------------------------- state monitor
struct Result {
    long long firstMs = -1;  // first endpoint reached desired state (-1 = none)
    long long allMs = -1;    // last endpoint reached desired state (-1 = not all)
};

class Monitor final : public IMMNotificationClient {
public:
    Monitor() = default;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = static_cast<ULONG>(--ref_);
        if (r == 0) delete this;
        return r;
    }

    // IMMNotificationClient
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState) override {
        if (deviceId) observe(deviceId, newState, "event");
        return S_OK;
    }
    STDMETHODIMP OnDeviceAdded(LPCWSTR deviceId) override {
        logLine("endpoint ADDED: " + narrow(deviceId ? deviceId : L""));
        return S_OK;
    }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR deviceId) override {
        logLine("endpoint REMOVED: " + narrow(deviceId ? deviceId : L""));
        return S_OK;
    }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    void arm(const std::vector<Endpoint>& eps) {
        std::lock_guard<std::mutex> lk(mu_);
        names_.clear(); cur_.clear(); startActive_.clear(); activeAt_.clear(); inactiveAt_.clear();
        drops_.clear();
        for (const auto& e : eps) {
            names_[e.id] = e.name;
            cur_[e.id] = e.state;
            startActive_[e.id] = (e.state == DEVICE_STATE_ACTIVE);
        }
        t0_ = Clock::now();
    }

    std::vector<std::wstring> ids() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::wstring> v;
        for (const auto& kv : names_) v.push_back(kv.first);
        return v;
    }

    // Records a state for a watched endpoint; logs only if it changed.
    void observe(const std::wstring& id, DWORD state, const char* source) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = names_.find(id);
        if (it == names_.end()) return;
        DWORD old = cur_[id];
        if (old == state) return;
        cur_[id] = state;
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0_).count();
        if (state == DEVICE_STATE_ACTIVE) {
            if (!activeAt_.count(id)) activeAt_[id] = ms;
        } else {
            if (!inactiveAt_.count(id)) inactiveAt_[id] = ms;
        }
        std::string name = narrow(it->second);
        if (old == DEVICE_STATE_ACTIVE && state != DEVICE_STATE_ACTIVE) {
            drops_.push_back(std::format("{} left ACTIVE -> {} at {}", name, stateName(state), wallStamp()));
        }
        logLine(std::format("+{} ms  {}: {} -> {} ({})", ms, name, stateName(old), stateName(state), source));
        cv_.notify_all();
    }

    // wantActive: wait until every endpoint that was not ACTIVE at arm() has become ACTIVE;
    // otherwise until every endpoint that was ACTIVE has left ACTIVE.
    bool waitAll(bool wantActive, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return doneLocked(wantActive); });
    }

    bool waitUntil(Clock::time_point deadline) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_until(lk, deadline);
        return true;
    }

    Result report(bool wantActive) {
        std::lock_guard<std::mutex> lk(mu_);
        Result r;
        bool anyNeeded = false, all = true;
        long long mn = -1, mx = -1;
        const auto& m = wantActive ? activeAt_ : inactiveAt_;
        for (const auto& kv : names_) {
            const std::wstring& id = kv.first;
            std::string name = narrow(kv.second);
            bool start = startActive_[id];
            bool need = wantActive ? !start : start;
            if (!need) {
                logLine(std::format("  {}: already {} at request time", name, wantActive ? "ACTIVE" : "not ACTIVE"));
                continue;
            }
            anyNeeded = true;
            auto it = m.find(id);
            if (it == m.end()) {
                all = false;
                logLine(std::format("  {}: NOT {} within timeout", name, wantActive ? "ACTIVE" : "left ACTIVE"));
            } else {
                logLine(std::format("  {}: {} after {} ms", name, wantActive ? "ACTIVE" : "left ACTIVE", it->second));
                if (mn < 0 || it->second < mn) mn = it->second;
                if (it->second > mx) mx = it->second;
            }
        }
        if (!anyNeeded) { r.firstMs = 0; r.allMs = 0; }
        else { r.firstMs = mn; r.allMs = all ? mx : -1; }
        return r;
    }

    std::vector<std::string> drops() {
        std::lock_guard<std::mutex> lk(mu_);
        return drops_;
    }

private:
    ~Monitor() = default;

    bool doneLocked(bool wantActive) {
        for (const auto& kv : names_) {
            const std::wstring& id = kv.first;
            bool start = startActive_[id];
            if (wantActive) {
                if (!start && !activeAt_.count(id)) return false;
            } else {
                if (start && !inactiveAt_.count(id)) return false;
            }
        }
        return true;
    }

    std::atomic<long> ref_{1};
    std::mutex mu_;
    std::condition_variable cv_;
    Clock::time_point t0_ = Clock::now();
    std::map<std::wstring, std::wstring> names_;
    std::map<std::wstring, DWORD> cur_;
    std::map<std::wstring, bool> startActive_;
    std::map<std::wstring, long long> activeAt_;
    std::map<std::wstring, long long> inactiveAt_;
    std::vector<std::string> drops_;
};

// Registers a Monitor for the lifetime of the object.
struct MonitorScope {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<Monitor> mon;
    bool registered = false;

    bool init() {
        HRESULT hr = S_OK;
        en = makeEnumerator(&hr);
        if (!en) { logLine("CoCreateInstance(MMDeviceEnumerator) failed " + hrs(hr)); return false; }
        mon.Attach(new Monitor());
        hr = en->RegisterEndpointNotificationCallback(mon.Get());
        if (FAILED(hr)) { logLine("RegisterEndpointNotificationCallback failed " + hrs(hr)); return false; }
        registered = true;
        return true;
    }
    // Fallback in case a notification is missed: read the actual state of each endpoint.
    void poll() {
        for (const auto& id : mon->ids()) {
            ComPtr<IMMDevice> dev;
            if (FAILED(en->GetDevice(id.c_str(), &dev))) continue;
            DWORD st = 0;
            if (SUCCEEDED(dev->GetState(&st))) mon->observe(id, st, "poll");
        }
    }
    ~MonitorScope() {
        if (registered && en && mon) en->UnregisterEndpointNotificationCallback(mon.Get());
    }
};

// ---------------------------------------------------------------- commands
int cmdList() {
    std::vector<Endpoint> eps;
    if (!enumerate(eps, L"", true)) return 1;
    logLine(std::format("{} endpoint(s)", eps.size()));
    for (const auto& e : eps) {
        logLine(std::format("{} | {} | {}", e.render ? "Render" : "Capture", narrow(e.name), stateName(e.state)));
        logLine("    id: " + narrow(e.id));
        if (e.ks) logLine("    KS: found; filter=" + narrow(e.filterKey) + "; " + e.ksNote);
        else logLine("    KS: not found; " + e.ksNote);
    }
    return 0;
}

// Returns false if the request could not be issued at all.
bool doRequest(const std::wstring& needleLower, bool connect, Result& result) {
    const char* verb = connect ? "RECONNECT" : "DISCONNECT";
    std::vector<Endpoint> eps;
    if (!enumerate(eps, needleLower, true)) return false;
    if (eps.empty()) { logLine("no endpoint matches '" + narrow(needleLower) + "'"); return false; }
    for (const auto& e : eps) {
        logLine(std::format("match: {} | {} | {} | KS {}", e.render ? "Render" : "Capture", narrow(e.name),
                            stateName(e.state), e.ks ? "ok" : ("missing: " + e.ksNote)));
    }

    MonitorScope scope;
    if (!scope.init()) return false;
    scope.mon->arm(eps);  // t0 = request time

    std::set<std::wstring> sent;
    int issued = 0;
    for (const auto& e : eps) {
        if (!e.ks) continue;
        if (!sent.insert(e.filterKey).second) continue;  // one request per distinct KS filter
        auto t = Clock::now();
        HRESULT hr = ksCall(e.ks.Get(), connect ? kBtReconnect : kBtDisconnect, KSPROPERTY_TYPE_GET);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
        logLine(std::format("ONESHOT_{} via '{}' (filter {}): hr={} (call took {} ms)", verb, narrow(e.name),
                            narrow(e.filterKey), hrs(hr), ms));
        ++issued;
    }
    if (issued == 0) { logLine("no KS filter available; nothing sent"); return false; }

    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < deadline) {
        if (scope.mon->waitAll(connect, std::chrono::milliseconds(500))) break;
        scope.poll();
        if (scope.mon->waitAll(connect, std::chrono::milliseconds(0))) break;
    }
    scope.poll();
    logLine(std::format("{} result:", connect ? "connect" : "disconnect"));
    result = scope.mon->report(connect);
    return true;
}

void printStats(const char* label, std::vector<long long> v, size_t total) {
    std::vector<long long> ok;
    for (auto x : v) if (x >= 0) ok.push_back(x);
    if (ok.empty()) {
        logLine(std::format("{}: no successful samples (0/{})", label, total));
        return;
    }
    std::sort(ok.begin(), ok.end());
    long long med = (ok.size() % 2 == 1) ? ok[ok.size() / 2] : (ok[ok.size() / 2 - 1] + ok[ok.size() / 2]) / 2;
    logLine(std::format("{}: n={}/{} min={} ms median={} ms max={} ms", label, ok.size(), total, ok.front(), med, ok.back()));
}

int cmdCycle(const std::wstring& needleLower, int count, int pauseSec) {
    std::vector<long long> dFirst, dAll, cFirst, cAll;
    for (int i = 1; i <= count; ++i) {
        logLine(std::format("=== cycle {}/{}: disconnect ===", i, count));
        Result d, c;
        if (!doRequest(needleLower, false, d)) { d = Result{}; }
        dFirst.push_back(d.firstMs); dAll.push_back(d.allMs);
        std::this_thread::sleep_for(std::chrono::seconds(pauseSec));
        logLine(std::format("=== cycle {}/{}: connect ===", i, count));
        if (!doRequest(needleLower, true, c)) { c = Result{}; }
        cFirst.push_back(c.firstMs); cAll.push_back(c.allMs);
        logLine(std::format("cycle {}: disconnect first={} all={} ms | connect first={} all={} ms (-1 = failed/timeout)",
                            i, d.firstMs, d.allMs, c.firstMs, c.allMs));
        std::this_thread::sleep_for(std::chrono::seconds(pauseSec));
    }
    logLine("=== summary ===");
    printStats("disconnect first-left-ACTIVE", dFirst, dFirst.size());
    printStats("disconnect all-left-ACTIVE  ", dAll, dAll.size());
    printStats("connect    first-ACTIVE     ", cFirst, cFirst.size());
    printStats("connect    all-ACTIVE       ", cAll, cAll.size());
    return 0;
}

// Optional: poll WinRT BluetoothDevice::ConnectionStatus for paired devices matching the name.
void btPoll(std::string needleLower, std::atomic<bool>& stop) {
    try {
        std::vector<std::pair<std::string, wbt::BluetoothDevice>> devs;
        auto infos = wde::DeviceInformation::FindAllAsync(wbt::BluetoothDevice::GetDeviceSelectorFromPairingState(true)).get();
        for (auto const& info : infos) {
            std::string n = winrt::to_string(info.Name());
            if (lowerA(n).find(needleLower) == std::string::npos) continue;
            auto dev = wbt::BluetoothDevice::FromIdAsync(info.Id()).get();
            if (dev) devs.emplace_back(n, dev);
        }
        if (devs.empty()) { logLine("BT poll: no paired BluetoothDevice matches"); return; }
        std::map<std::string, int> last;
        while (!stop.load()) {
            for (auto& d : devs) {
                int st = static_cast<int>(d.second.ConnectionStatus());
                auto it = last.find(d.first);
                if (it == last.end() || it->second != st) {
                    logLine(std::format("BT ConnectionStatus {}: {}", d.first,
                                        st == static_cast<int>(wbt::BluetoothConnectionStatus::Connected) ? "Connected" : "Disconnected"));
                    last[d.first] = st;
                }
            }
            for (int i = 0; i < 20 && !stop.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    } catch (const winrt::hresult_error& e) {
        logLine(std::format("BT poll failed: {} {}", hrs(e.code()), winrt::to_string(e.message())));
    } catch (const std::exception& e) {
        logLine(std::string("BT poll failed: ") + e.what());
    }
}

int cmdWatch(const std::wstring& needleLower, double minutes) {
    std::vector<Endpoint> eps;
    if (!enumerate(eps, needleLower, false)) return 1;
    if (eps.empty()) { logLine("no endpoint matches '" + narrow(needleLower) + "'"); return 1; }
    for (const auto& e : eps) {
        logLine(std::format("watching: {} | {} | {}", e.render ? "Render" : "Capture", narrow(e.name), stateName(e.state)));
    }
    MonitorScope scope;
    if (!scope.init()) return 1;
    scope.mon->arm(eps);

    std::atomic<bool> stop{false};
    std::thread poller(btPoll, lowerA(narrow(needleLower)), std::ref(stop));

    auto deadline = Clock::now() + std::chrono::milliseconds(static_cast<long long>(minutes * 60.0 * 1000.0));
    while (Clock::now() < deadline) {
        auto next = std::min(deadline, Clock::now() + std::chrono::seconds(5));
        scope.mon->waitUntil(next);
        scope.poll();
    }
    stop.store(true);
    poller.join();

    auto drops = scope.mon->drops();
    logLine(std::format("=== watch summary: {} drop(s) from ACTIVE in {} min ===", drops.size(), minutes));
    for (const auto& d : drops) logLine("  " + d);
    return 0;
}

void usage() {
    std::puts(
        "usage:\n"
        "  ks_connect_spike list\n"
        "  ks_connect_spike connect    <name-substring>\n"
        "  ks_connect_spike disconnect <name-substring>\n"
        "  ks_connect_spike cycle      <name-substring> <count> [pauseSeconds=5]\n"
        "  ks_connect_spike watch      <name-substring> <minutes>");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        g_logFile.open("ks_connect_spike.log", std::ios::app);
        if (argc < 2) { usage(); return 2; }
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) { logLine("CoInitializeEx failed " + hrs(hr)); return 1; }

        std::wstring cmd = lowerW(argv[1]);
        std::wstring needle = argc >= 3 ? lowerW(argv[2]) : L"";
        logLine("ks_connect_spike " + narrow(cmd) + (needle.empty() ? "" : " '" + narrow(needle) + "'"));

        int rc = 2;
        if (cmd == L"list") {
            rc = cmdList();
        } else if (cmd == L"connect" || cmd == L"disconnect") {
            if (needle.empty()) { usage(); }
            else {
                Result r;
                bool ok = doRequest(needle, cmd == L"connect", r);
                rc = ok ? 0 : 1;
            }
        } else if (cmd == L"cycle") {
            if (argc < 4 || needle.empty()) { usage(); }
            else {
                int count = std::stoi(argv[3]);
                int pause = argc >= 5 ? std::stoi(argv[4]) : 5;
                if (count < 1 || pause < 0) { usage(); }
                else rc = cmdCycle(needle, count, pause);
            }
        } else if (cmd == L"watch") {
            if (argc < 4 || needle.empty()) { usage(); }
            else {
                double minutes = std::stod(argv[3]);
                if (minutes <= 0) { usage(); }
                else rc = cmdWatch(needle, minutes);
            }
        } else {
            usage();
        }
        CoUninitialize();
        return rc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "fatal: unknown exception\n");
        return 1;
    }
}
