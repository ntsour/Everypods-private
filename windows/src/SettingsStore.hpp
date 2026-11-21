#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Propods {

struct Settings {
    std::vector<std::uint64_t>   androidAddresses;  // one entry per paired Android peer
    std::optional<std::uint64_t> airpodsAddress;
    bool                         autoRoute{true};   // auto-set AirPods as default audio device
    bool                         pauseOnRelease{true};  // pause/mute PC audio when the AirPods leave
    bool                         takeOnMediaStart{true};  // take the AirPods when this PC starts playing media
    bool                         takeOnCall{true};        // take the AirPods when a call starts on this PC
};

class SettingsStore {
public:
    SettingsStore();
    Settings load() const;
    void save(const Settings& s) const;

    static std::optional<std::uint64_t> parseBluetoothAddress(std::string_view s);
    static std::string formatBluetoothAddress(std::uint64_t addr);

private:
    std::filesystem::path m_path;
};

}
