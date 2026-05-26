#pragma once

namespace Propods {

// Toggles the mute state of the default eCommunications capture device
// (i.e. the microphone Windows uses for calls / VoIP). Lightweight:
// creates and releases COM objects on each call — no long-lived state.
class MicMuter {
public:
    void toggle();
    bool isMuted() const;
};

} // namespace Propods
