// OutputMute.swift
//
// Mutes the current default output (the speakers, never the AirPods) while a playback hold connects the
// AirPods, and restores it afterwards. The muted device is remembered in UserDefaults first, so a crash or
// quit while muted is undone on the next launch (`recoverIfNeeded`). Uses the device's mute switch when it
// has one, otherwise its virtual main volume (restored to the saved level).

import AudioToolbox
import CoreAudio
import Foundation

enum OutputMute {
    private static let defaults = UserDefaults.standard
    private static let uidKey = "holdMutedDeviceUID"
    private static let volumeKey = "holdMutedDeviceVolume"   // only set when muted by volume

    /// True while this app has a device muted.
    static var isMuted: Bool { defaults.string(forKey: uidKey) != nil }

    /// Mutes the default output unless it is the AirPods or already muted. Returns true if it muted something.
    @discardableResult
    static func muteDefaultOutput(unlessAddress airPodsAddress: String?) -> Bool {
        guard !isMuted, let device = defaultOutput() else { return false }
        let uid = string(device, kAudioDevicePropertyDeviceUID)
        if let address = airPodsAddress, uid.uppercased().hasPrefix(normalizedAddress(address).uppercased()) {
            return false   // already on the AirPods: nothing to hide
        }
        if hasMute(device) {
            guard !getMute(device) else { return false }   // the user muted it: leave it alone
            defaults.set(uid, forKey: uidKey)
            setMute(device, true)
        } else if let volume = getVolume(device) {
            guard volume > 0 else { return false }
            defaults.set(uid, forKey: uidKey)
            defaults.set(volume, forKey: volumeKey)
            setVolume(device, 0)
        } else {
            return false
        }
        Log.write("Hold: muted \(uid)")
        return true
    }

    /// Restores whatever `muteDefaultOutput` muted (no-op when nothing is muted).
    static func restore(_ reason: String) {
        guard let uid = defaults.string(forKey: uidKey) else { return }
        defer {
            defaults.removeObject(forKey: uidKey)
            defaults.removeObject(forKey: volumeKey)
        }
        guard let device = device(forUID: uid) else {
            return Log.write("Hold: muted device \(uid) is gone; nothing to restore (\(reason))")
        }
        if let volume = defaults.object(forKey: volumeKey) as? Float {
            setVolume(device, volume)
        } else {
            setMute(device, false)
        }
        Log.write("Hold: restored \(uid) (\(reason))")
    }

    /// At launch: undo a mute left behind by a crash or quit.
    static func recoverIfNeeded() {
        if isMuted { restore("left muted by a previous run") }
    }

    // MARK: CoreAudio

    private static func address(_ selector: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    }

    private static func defaultOutput() -> AudioDeviceID? {
        var device = AudioDeviceID(0)
        var addr = address(kAudioHardwarePropertyDefaultOutputDevice)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &device)
        return status == noErr && device != kAudioObjectUnknown ? device : nil
    }

    private static func device(forUID uid: String) -> AudioDeviceID? {
        var cfUID = uid as CFString
        var device = AudioDeviceID(0)
        var addr = address(kAudioHardwarePropertyTranslateUIDToDevice)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = withUnsafeMutablePointer(to: &cfUID) {
            AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr,
                                       UInt32(MemoryLayout<CFString>.size), $0, &size, &device)
        }
        return status == noErr && device != kAudioObjectUnknown ? device : nil
    }

    private static func string(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector) -> String {
        var addr = address(selector)
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(object, &addr, 0, nil, &size, &value) == noErr, let value else { return "?" }
        return value.takeRetainedValue() as String
    }

    private static func hasMute(_ device: AudioDeviceID) -> Bool {
        var addr = address(kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput)
        var settable: DarwinBoolean = false
        return AudioObjectHasProperty(device, &addr)
            && AudioObjectIsPropertySettable(device, &addr, &settable) == noErr && settable.boolValue
    }

    private static func getMute(_ device: AudioDeviceID) -> Bool {
        var addr = address(kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput)
        var value: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        return AudioObjectGetPropertyData(device, &addr, 0, nil, &size, &value) == noErr && value != 0
    }

    private static func setMute(_ device: AudioDeviceID, _ on: Bool) {
        var addr = address(kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput)
        var value: UInt32 = on ? 1 : 0
        AudioObjectSetPropertyData(device, &addr, 0, nil, UInt32(MemoryLayout<UInt32>.size), &value)
    }

    private static func getVolume(_ device: AudioDeviceID) -> Float? {
        var addr = address(kAudioHardwareServiceDeviceProperty_VirtualMainVolume, kAudioDevicePropertyScopeOutput)
        guard AudioObjectHasProperty(device, &addr) else { return nil }
        var value: Float32 = 0
        var size = UInt32(MemoryLayout<Float32>.size)
        return AudioObjectGetPropertyData(device, &addr, 0, nil, &size, &value) == noErr ? value : nil
    }

    private static func setVolume(_ device: AudioDeviceID, _ volume: Float) {
        var addr = address(kAudioHardwareServiceDeviceProperty_VirtualMainVolume, kAudioDevicePropertyScopeOutput)
        var value = Float32(volume)
        AudioObjectSetPropertyData(device, &addr, 0, nil, UInt32(MemoryLayout<Float32>.size), &value)
    }
}
