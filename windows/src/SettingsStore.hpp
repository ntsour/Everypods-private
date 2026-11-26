#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "TriggerApps.hpp"

namespace Propods {

struct Settings {
    // Every configured peer, deduplicated. There is no phone/computer distinction.
    // The name is historical: it is the key older builds read from config.txt.
    std::vector<std::uint64_t>   peerAddresses;
    std::optional<std::uint64_t> airpodsAddress;
    bool                         autoRoute{true};   // auto-set AirPods as default audio device
    bool                         pauseOnRelease{true};  // pause/mute PC audio when the AirPods leave
    bool                         takeOnMediaStart{true};  // take the AirPods when this PC starts playing media
    bool                         takeOnCall{true};        // take the AirPods when a call starts on this PC
    bool                         holdPlayback{false};     // hold PC audio while the AirPods connect (PlaybackHold)
    bool                         lidOpenConnect{true};    // connect when the case opens, if this PC used them last
    bool                         lidLease{false};         // this PC used the AirPods last (lid-open connect)
    // Apps the user added ("takeonmic=" / "takeonplay=" / "keeponmic=" / "nevertake=" lines, one per setting).
    std::vector<TriggerApp>      triggerApps;
    // Apps that used sound or the mic, most recent first, at most 40
    // ("seenapp=" lines): the tray lists them across restarts.
    std::vector<std::wstring>    seenApps;
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
