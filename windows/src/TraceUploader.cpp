#include "TraceUploader.hpp"

#include "Logger.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace Propods {

namespace {

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Rfcomm;
using namespace winrt::Windows::Networking::Sockets;
using namespace winrt::Windows::Storage::Streams;

// Must match TraceUploadReceiver.SERVICE_UUID on Android.
constexpr winrt::guid kTraceUploadService{
    0x662d622f, 0x1c89, 0x4324, {0x9f, 0x28, 0x07, 0xd4, 0xfd, 0x52, 0x19, 0x82}};
constexpr std::uintmax_t kMaxFileBytes = std::uintmax_t{256} * 1024 * 1024;

std::vector<std::filesystem::path> recentTraces(std::chrono::hours maxAge) {
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (const auto& it : fs::directory_iterator(log::traceDir(), ec)) {
        const auto name = it.path().filename().wstring();
        if (!it.is_regular_file(ec) || !name.starts_with(L"windows-") || it.path().extension() != L".log")
            continue;
        const auto t = it.last_write_time(ec);
        if (!ec && now - t <= maxAge) files.push_back(it.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace

TraceUploadResult uploadTraces(std::uint64_t peerAddress, std::chrono::hours maxAge) {
    TraceUploadResult result;
    try {
        auto device = BluetoothDevice::FromBluetoothAddressAsync(peerAddress).get();
        if (!device) {
            result.error = "phone not found (is it paired?)";
            return result;
        }
        auto services = device.GetRfcommServicesForIdAsync(
            RfcommServiceId::FromUuid(kTraceUploadService), BluetoothCacheMode::Uncached).get();
        if (services.Services().Size() == 0) {
            result.error = "doesn't offer EveryPods trace upload (not running EveryPods, or "
                           "\"Full handover trace\" is off in its Debug screen)";
            return result;
        }
        auto service = services.Services().GetAt(0);
        StreamSocket socket;
        socket.ConnectAsync(service.ConnectionHostName(), service.ConnectionServiceName()).get();
        DataWriter writer{socket.OutputStream()};  // ByteOrder defaults to big-endian
        DataReader reader{socket.InputStream()};

        const auto files = recentTraces(maxAge);
        log::info("Trace upload to {:012X}: {} file(s)", peerAddress, files.size());
        std::vector<std::uint8_t> buffer(64 * 1024);
        for (const auto& path : files) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);  // the current run keeps growing; send what exists now
            const auto name = log::toUtf8(path.filename().wstring());
            if (ec || size > kMaxFileBytes) {
                log::warn("Trace upload: skipping {} ({})", name, ec ? ec.message() : "too large");
                continue;
            }
            std::ifstream in{path, std::ios::binary};
            if (!in) {
                log::warn("Trace upload: cannot open {}", name);
                continue;
            }
            writer.WriteUInt32(static_cast<std::uint32_t>(name.size()));
            writer.WriteBytes(winrt::array_view<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(name.data()),
                reinterpret_cast<const std::uint8_t*>(name.data()) + name.size()));
            writer.WriteUInt32(static_cast<std::uint32_t>(size));
            std::uintmax_t remaining = size;
            while (remaining > 0) {
                const auto want = static_cast<std::streamsize>(std::min<std::uintmax_t>(buffer.size(), remaining));
                in.read(reinterpret_cast<char*>(buffer.data()), want);
                const auto got = in.gcount();
                if (got <= 0) throw std::runtime_error("trace file ended early: " + name);
                writer.WriteBytes(winrt::array_view<const std::uint8_t>(buffer.data(), buffer.data() + got));
                writer.StoreAsync().get();
                remaining -= static_cast<std::uintmax_t>(got);
            }
            writer.StoreAsync().get();
            if (reader.LoadAsync(1).get() != 1) throw std::runtime_error("phone closed the connection");
            if (reader.ReadByte() == 1) {
                ++result.stored;
                log::info("Trace upload: {} ({} bytes) stored", name, size);
            } else {
                ++result.rejected;
                log::warn("Trace upload: {} rejected by phone", name);
            }
        }
        writer.WriteUInt32(0);
        writer.StoreAsync().get();
        socket.Close();
    } catch (const winrt::hresult_error& e) {
        result.error = std::format("Bluetooth error 0x{:08X} {}",
            static_cast<std::uint32_t>(e.code().value), winrt::to_string(e.message()));
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    if (!result.error.empty())
        log::warn("Trace upload to {:012X} failed: {}", peerAddress, result.error);
    return result;
}

}
