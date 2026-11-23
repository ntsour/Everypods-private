// AudioRouter.swift
//
// Makes the AirPods the default audio output when this Mac gets them (the "Auto-set AirPods as audio device"
// option of the Windows client). macOS usually does this itself; the router only acts when it did not, and
// keeps re-applying while this Mac holds the AirPods (a brief Bluetooth blip can drop the default).

import CoreAudio
import Foundation

final class AudioRouter {
    private var persistent = false
    private var listener: AudioObjectPropertyListenerBlock?

    init() {
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            guard let self, self.persistent, Settings.autoRoute else { return }
            self.routeToAirPods()
        }
        listener = block
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, DispatchQueue.main, block)
    }

    /// While true, the AirPods are re-selected whenever the device list changes.
    func setPersistent(_ value: Bool) { persistent = value }

    /// Selects the AirPods as the output (and system sounds) device unless they already are.
    @discardableResult
    func routeToAirPods() -> Bool {
        guard let address = Settings.airPodsAddress, let device = outputDevice(forAddress: address) else { return false }
        guard defaultOutput() != device else { return true }
        let ok = setDefaultOutput(device, selector: kAudioHardwarePropertyDefaultOutputDevice)
            && setDefaultOutput(device, selector: kAudioHardwarePropertyDefaultSystemOutputDevice)
        Log.write("Audio: set AirPods as the default output (\(ok ? "ok" : "failed"))")
        return ok
    }

    /// True when the AirPods exist as an audio output and are the default one.
    var airPodsAreDefaultOutput: Bool {
        guard let address = Settings.airPodsAddress, let device = outputDevice(forAddress: address) else { return false }
        return defaultOutput() == device
    }

    // MARK: CoreAudio

    private func outputDevice(forAddress address: String) -> AudioDeviceID? {
        // Bluetooth devices have the UID "AA-BB-CC-DD-EE-FF:output".
        var uid = (normalizedAddress(address).uppercased() + ":output") as CFString
        var device = AudioDeviceID(0)
        var translate = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyTranslateUIDToDevice,
                                                   mScope: kAudioObjectPropertyScopeGlobal,
                                                   mElement: kAudioObjectPropertyElementMain)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = withUnsafeMutablePointer(to: &uid) {
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &translate,
                                       UInt32(MemoryLayout<CFString>.size), $0, &size, &device)
        }
        return status == noErr && device != kAudioObjectUnknown ? device : nil
    }

    private func defaultOutput() -> AudioDeviceID? {
        var device = AudioDeviceID(0)
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultOutputDevice,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &device)
        return status == noErr ? device : nil
    }

    private func setDefaultOutput(_ device: AudioDeviceID, selector: AudioObjectPropertySelector) -> Bool {
        var value = device
        var address = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        return AudioObjectSetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil,
                                          UInt32(MemoryLayout<AudioDeviceID>.size), &value) == noErr
    }
}
