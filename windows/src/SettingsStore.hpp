#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Propods {

struct Settings {
    // Every configured peer, deduplicated. There is no phone/computer distinction.
    // The name is historical: it is the key older builds read from config.txt.
    std::vector<std::uint64_t>   peerAddresses;
    std::optional<std::uint64_t> airpodsAddress;
    bool                         autoRoute{true};   // auto-set AirPods as default audio device
    bool                         pauseOnRelease{true};  // pause/mute PC audio when the AirPods leave
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
