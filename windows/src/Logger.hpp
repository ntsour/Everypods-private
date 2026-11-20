#pragma once

#include <windows.h>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Propods::log {

inline std::mutex& mutex() {
    static std::mutex m;
    return m;
}

inline std::filesystem::path logDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    auto dir = (len > 0 ? std::filesystem::path{buf} : std::filesystem::temp_directory_path()) / L"Propods";
    std::filesystem::create_directories(dir);
    return dir;
}

inline std::ofstream& logFile() {
    static std::ofstream f = []() {
        return std::ofstream{logDir() / "Propods.log", std::ios::trunc};
    }();
    return f;
}

inline std::ofstream& handoverLogFile() {
    static std::ofstream f = []() {
        return std::ofstream{logDir() / "handover.log", std::ios::trunc};
    }();
    return f;
}

// ── Durable trace ───────────────────────────────────────────────────────────
// Propods.log / handover.log are truncated on every launch. The trace is the
// durable record: one file per process run under
// %LOCALAPPDATA%\EveryPods\traces, opened in append mode, never truncated or
// rotated while running. It receives every log line at every level (including
// DEBUG in Release builds) with a full local date, milliseconds and thread id.
// Old runs are pruned only at startup: files older than 14 days, then oldest
// first while the folder exceeds 2 GiB. The current run is never pruned.
inline constexpr auto kTraceRetention = std::chrono::hours{24 * 14};
inline constexpr std::uintmax_t kTraceMaxTotalBytes = std::uintmax_t{2} << 30;

inline std::string toUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

inline std::filesystem::path traceDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::filesystem::path base = (len > 0 && len < MAX_PATH)
        ? std::filesystem::path{buf}
        : std::filesystem::temp_directory_path();
    auto dir = base / L"EveryPods" / L"traces";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

inline void pruneTraces(const std::filesystem::path& dir, const std::filesystem::path& keep) {
    namespace fs = std::filesystem;
    struct Entry { fs::path path; fs::file_time_type time; std::uintmax_t size; };
    std::vector<Entry> files;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (const auto& it : fs::directory_iterator(dir, ec)) {
        if (!it.is_regular_file(ec) || it.path().extension() != L".log") continue;
        if (it.path() == keep) continue;
        const auto t = it.last_write_time(ec);
        if (ec) continue;
        if (now - t > kTraceRetention) {
            fs::remove(it.path(), ec);
            continue;
        }
        files.push_back({it.path(), t, it.file_size(ec)});
    }
    std::uintmax_t total = 0;
    for (const auto& f : files) total += f.size;
    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b) { return a.time < b.time; });
    for (const auto& f : files) {
        if (total <= kTraceMaxTotalBytes) break;
        if (fs::remove(f.path, ec)) total -= f.size;
    }
}

inline std::string fullTimestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    TIME_ZONE_INFORMATION tz{};
    const DWORD tzState = GetTimeZoneInformation(&tz);
    long bias = tz.Bias;  // minutes, UTC = local + bias
    if (tzState == TIME_ZONE_ID_DAYLIGHT) bias += tz.DaylightBias;
    else if (tzState == TIME_ZONE_ID_STANDARD) bias += tz.StandardBias;
    const long offset = -bias;
    const long absOffset = offset < 0 ? -offset : offset;
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} {}{:02}:{:02}",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        st.wMilliseconds, offset < 0 ? '-' : '+', absOffset / 60, absOffset % 60);
}

inline std::filesystem::path& tracePath() {
    static std::filesystem::path p;
    return p;
}

inline std::ofstream& traceFile() {
    static std::ofstream f = []() {
        const auto dir = traceDir();
        SYSTEMTIME st{};
        GetLocalTime(&st);
        const auto name = std::format(L"windows-{:04}{:02}{:02}-{:02}{:02}{:02}-pid{}.log",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            GetCurrentProcessId());
        tracePath() = dir / name;
        pruneTraces(dir, tracePath());
        std::ofstream out{tracePath(), std::ios::app | std::ios::binary};
        if (out.is_open()) {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            out << std::format("{} T{:05} TRACE trace started pid={} exe={}\n",
                fullTimestamp(), GetCurrentThreadId(), GetCurrentProcessId(), toUtf8(exe));
            out.flush();
        }
        return out;
    }();
    return f;
}

inline void writeTrace(std::string_view level, std::string_view msg) {
    if (auto& t = traceFile(); t.is_open()) {
        t << std::format("{} T{:05} {} {}\n", fullTimestamp(), GetCurrentThreadId(), level, msg);
        t.flush();
    }
}

inline std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    try {
        // current_zone() requires Windows 10 1903+ and the tzdata package.
        // Fall back to UTC on older systems or if the database is missing.
        const auto local = zoned_time{current_zone(), now}.get_local_time();
        return std::format("{:%H:%M:%S}", floor<milliseconds>(local));
    } catch (...) {
        return std::format("{:%H:%M:%S}", floor<milliseconds>(now));
    }
}

inline void writeLine(std::string_view level, std::string_view msg,
                      std::ofstream* extra = nullptr) {
    const auto ts = timestamp();
    const auto line = std::format("[{}] {} {}\n", ts, level, msg);
    writeTrace(level, msg);
    std::cerr << line;
    if (auto& f = logFile(); f.is_open()) {
        f << line;
        f.flush();
    }
    if (extra && extra->is_open()) {
        *extra << line;
        extra->flush();
    }
}

template <class... Args>
void info(std::string_view fmt, Args&&... args) {
    std::scoped_lock lk{mutex()};
    writeLine("INFO ", std::vformat(fmt, std::make_format_args(args...)));
}

template <class... Args>
void warn(std::string_view fmt, Args&&... args) {
    std::scoped_lock lk{mutex()};
    writeLine("WARN ", std::vformat(fmt, std::make_format_args(args...)));
}

template <class... Args>
void error(std::string_view fmt, Args&&... args) {
    std::scoped_lock lk{mutex()};
    writeLine("ERROR", std::vformat(fmt, std::make_format_args(args...)));
}

// Writes to both the main log and the dedicated handover.log.
// Use for every decision point in the handover state machine so the handover
// log gives a clean, noise-free timeline without endpoint enumeration spam.
template <class... Args>
void handover(std::string_view fmt, Args&&... args) {
    std::scoped_lock lk{mutex()};
    writeLine("HNDOVR", std::vformat(fmt, std::make_format_args(args...)),
              &handoverLogFile());
}

template <class... Args>
void debug(std::string_view fmt, Args&&... args) {
    std::scoped_lock lk{mutex()};
#ifndef NDEBUG
    writeLine("DEBUG", std::vformat(fmt, std::make_format_args(args...)));
#else
    // Release builds keep DEBUG out of the console and Propods.log, but the
    // durable trace records it (packet hex dumps, connect steps).
    writeTrace("DEBUG", std::vformat(fmt, std::make_format_args(args...)));
#endif
}

}
