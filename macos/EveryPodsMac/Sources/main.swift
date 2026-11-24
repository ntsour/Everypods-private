// main.swift
//
// Menu-bar app: the Mac counterpart of the Windows tray icon (windows/src/TrayIcon.cpp, main.cpp).
// Shows who holds the AirPods, the paired phones, and the same actions and toggles.

import AppKit
import IOBluetooth
import ServiceManagement

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let controller = HandoverController()
    private var gattAuthorizer: GattSetupAuthorizer?
    private var menuCandidates: [PeerRegistry.Candidate] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Single instance: if another copy is already running, quit silently.
        if let id = Bundle.main.bundleIdentifier,
           NSRunningApplication.runningApplications(withBundleIdentifier: id).contains(where: { $0 != .current }) {
            NSApp.terminate(nil)
            return
        }

        statusItem.button?.image = NSImage(systemSymbolName: "airpodspro", accessibilityDescription: "EveryPods")
        if statusItem.button?.image == nil { statusItem.button?.title = "EP" }
        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu

        firstRunSetup()
        controller.onChange = { [weak self] in self?.updateIcon() }
        controller.start()
        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in
            self?.controller.handleWake()
        }
        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { [weak self] _ in
            self?.controller.handleSleep()
        }
        updateIcon()
        Log.write("EveryPods for Mac started")
    }

    /// First launch: pick up the paired AirPods and any paired phone that runs the Android app.
    private func firstRunSetup() {
        if Settings.airPodsAddress == nil, let airPods = PeerDiscovery.findAirPods().first {
            Settings.airPodsAddress = airPods.address
            Log.write("Setup: selected AirPods \(airPods.name)")
        }
        guard !Settings.didFirstRun || (Settings.peers.isEmpty) else { return }
        // Give the Bluetooth LE links a moment to come up: phones found there are authorized over LE.
        DispatchQueue.main.asyncAfter(deadline: .now() + 4) { [weak self] in self?.firstRunPeers() }
    }

    private func firstRunPeers() {
        for candidate in controller.peers.discoverable {
            guard let address = candidate.address, !isKnownPeer(address) else { continue }
            Log.write("Setup: found Bluetooth LE peer \(candidate.name)")
            addAndAuthorize(address: address, name: candidate.name, installationId: candidate.installationId, silent: true)
        }
        Settings.didFirstRun = true
        if Settings.peers.isEmpty {
            Log.write("Setup: no phone found over Bluetooth LE")
        }
    }

    private func isKnownPeer(_ address: String) -> Bool {
        Settings.peers.contains { sameAddress($0, address) }
    }

    private func updateIcon() {
        statusItem.button?.appearsDisabled = controller.ownership != .thisMac
        statusItem.button?.toolTip = controller.airPodsStatus
    }

    // MARK: Menu (rebuilt every time it opens, so it always shows current state)

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()

        menu.addItem(disabled(controller.airPodsStatus))
        let connected = disabled(controller.pods.isConnected ? "AirPods connected (Bluetooth)" : "AirPods not connected")
        connected.state = controller.pods.isConnected ? .on : .off
        menu.addItem(connected)
        menu.addItem(disabled(controller.macAudioStatus))
        menu.addItem(.separator())

        let infos = controller.peers.infos
        if !infos.isEmpty {
            menu.addItem(disabled("Peers:"))
            for info in infos {
                let item = NSMenuItem(title: info.name, action: nil, keyEquivalent: "")
                item.state = info.connected ? .on : .off
                let submenu = NSMenu()
                let authorize = NSMenuItem(title: "Authorize with Phone…", action: #selector(authorizePeer(_:)), keyEquivalent: "")
                authorize.target = self
                authorize.representedObject = info.address
                submenu.addItem(authorize)
                let remove = NSMenuItem(title: "Remove \(info.name)", action: #selector(removePeer(_:)), keyEquivalent: "")
                remove.target = self
                remove.representedObject = info.address
                submenu.addItem(remove)
                item.submenu = submenu
                menu.addItem(item)
            }
            menu.addItem(.separator())
        }

        menuCandidates = controller.peers.discoverable
        if menuCandidates.isEmpty {
            menu.addItem(action("Add peer…", #selector(addPeer)))
        } else {
            let add = NSMenuItem(title: "Add peer", action: nil, keyEquivalent: "")
            let submenu = NSMenu()
            for (index, candidate) in menuCandidates.enumerated() {
                if let address = candidate.address {
                    let item = NSMenuItem(title: "\(candidate.name) (\(address))", action: #selector(addCandidate(_:)), keyEquivalent: "")
                    item.target = self
                    item.tag = index
                    submenu.addItem(item)
                } else {
                    submenu.addItem(disabled("\(candidate.name): Pair this phone with the Mac in System Settings first"))
                }
            }
            add.submenu = submenu
            menu.addItem(add)
        }
        menu.addItem(selectAirPodsMenu())
        let connect = action("Connect AirPods now", #selector(connectNow))
        connect.isEnabled = controller.pods.isConfigured && !controller.pods.isConnected
        menu.addItem(connect)
        let release = action("Release AirPods", #selector(releaseTapped))
        release.isEnabled = controller.pods.isConnected
        menu.addItem(release)
        menu.addItem(.separator())

        menu.addItem(toggle("Take over when media starts", #selector(toggleTakeMedia), Settings.takeOnMediaStart))
        menu.addItem(toggle("Take over when receiving a call", #selector(toggleTakeCall), Settings.takeOnCall))
        menu.addItem(toggle("Auto-set AirPods as audio device", #selector(toggleAutoRoute), Settings.autoRoute))
        menu.addItem(toggle("Pause playback when handing over", #selector(togglePause), Settings.pauseOnHandover))
        menu.addItem(toggle("Connect AirPods when the case opens (if this Mac used them last)", #selector(toggleLidOpen),
                            Settings.lidOpenConnect))
        if !MediaControl.hasPermission {
            menu.addItem(action("Allow Pausing Playback (Accessibility)…", #selector(grantAccessibility)))
        }
        menu.addItem(.separator())

        menu.addItem(toggle("Start at login", #selector(toggleLaunchAtLogin), SMAppService.mainApp.status == .enabled))
        menu.addItem(action("Show Log", #selector(showLog)))
        menu.addItem(NSMenuItem(title: "Quit EveryPods", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
    }

    private func disabled(_ title: String) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.isEnabled = false
        return item
    }

    private func action(_ title: String, _ selector: Selector) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: selector, keyEquivalent: "")
        item.target = self
        return item
    }

    private func toggle(_ title: String, _ selector: Selector, _ on: Bool) -> NSMenuItem {
        let item = action(title, selector)
        item.state = on ? .on : .off
        return item
    }

    private func selectAirPodsMenu() -> NSMenuItem {
        let parent = NSMenuItem(title: "Select AirPods", action: nil, keyEquivalent: "")
        let submenu = NSMenu()
        for device in Settings.pairedDevices() {
            guard let address = device.addressString else { continue }
            let item = NSMenuItem(title: device.name ?? address, action: #selector(chooseAirPods(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = address
            item.state = Settings.airPodsAddress.map { sameAddress($0, address) } == true ? .on : .off
            submenu.addItem(item)
        }
        if submenu.items.isEmpty { submenu.addItem(disabled("No paired devices")) }
        parent.submenu = submenu
        return parent
    }

    // MARK: Actions

    @objc private func connectNow() { controller.connectNow() }
    @objc private func releaseTapped() { controller.releaseAirPods() }
    @objc private func toggleTakeMedia() { Settings.takeOnMediaStart.toggle() }
    @objc private func toggleTakeCall() { Settings.takeOnCall.toggle() }
    @objc private func toggleAutoRoute() { Settings.autoRoute.toggle() }
    @objc private func togglePause() { Settings.pauseOnHandover.toggle() }
    @objc private func toggleLidOpen() {
        Settings.lidOpenConnect.toggle()
        Log.write("Connect AirPods when the case opens (last device): \(Settings.lidOpenConnect ? "on" : "off")")
    }
    @objc private func grantAccessibility() { MediaControl.requestPermission() }

    @objc private func chooseAirPods(_ sender: NSMenuItem) {
        Settings.airPodsAddress = sender.representedObject as? String
        controller.onChange?()
    }

    @objc private func removePeer(_ sender: NSMenuItem) {
        guard let address = sender.representedObject as? String else { return }
        controller.peers.remove(address)
    }

    @objc private func authorizePeer(_ sender: NSMenuItem) {
        guard let address = sender.representedObject as? String else { return }
        runAuthorization(for: address, name: IOBluetoothDevice(addressString: normalizedAddress(address))?.name ?? address, silent: false)
    }

    /// "Add peer…" while no phone is visible over Bluetooth LE.
    @objc private func addPeer() {
        alert("No phone found",
              "Make sure the phone is paired in System Settings → Bluetooth and runs EveryPods (on Android, with Cross-Device Handover on), then try again.")
    }

    /// A phone found over Bluetooth LE, picked from the "Add peer" submenu.
    @objc private func addCandidate(_ sender: NSMenuItem) {
        guard menuCandidates.indices.contains(sender.tag), let address = menuCandidates[sender.tag].address else { return }
        let candidate = menuCandidates[sender.tag]
        guard !isKnownPeer(address), confirmAdd(name: candidate.name, address: address) else { return }
        addAndAuthorize(address: address, name: candidate.name, installationId: candidate.installationId, silent: false)
    }

    private func confirmAdd(name: String, address: String) -> Bool {
        let ask = NSAlert()
        ask.messageText = "Add as peer?"
        ask.informativeText = "\(name)\n\(address)"
        ask.addButton(withTitle: "Add")
        ask.addButton(withTitle: "Cancel")
        NSApp.activate(ignoringOtherApps: true)
        return ask.runModal() == .alertFirstButtonReturn
    }

    private func addAndAuthorize(address: String, name: String, installationId: String?, silent: Bool) {
        controller.peers.add(address)
        if let installationId { controller.peers.remember(installationId: installationId, address: address) }
        runAuthorization(for: address, name: name, silent: silent, installationId: installationId)
    }

    /// The one-off setup exchange newer Android builds require before they accept this Mac, over the Bluetooth LE link.
    private func runAuthorization(for address: String, name: String, silent: Bool, installationId: String? = nil) {
        guard gattAuthorizer == nil else { return }
        let id = installationId ?? controller.peers.leInstallationId(for: address)
        guard let id, controller.peers.hasLESession(id) else {
            Log.write("Authorization with \(name): no Bluetooth LE link")
            if !silent {
                alert("Authorization with \(name) failed",
                      "The phone has no Bluetooth LE link to this Mac. Check that Cross-Device Handover is on in the Android app, then use the phone's menu → Authorize with Phone….")
            }
            return
        }
        controller.peers.remember(installationId: id, address: address)
        let gatt = GattSetupAuthorizer(registry: controller.peers, installationId: id)
        gattAuthorizer = gatt
        gatt.run { [weak self] result in
            self?.gattAuthorizer = nil
            switch result {
            case .success: self?.authorized(name: name, silent: silent)
            case .failure(let error):
                Log.write("Authorization with \(name) failed: \(error.localizedDescription)")
                if !silent {
                    self?.alert("Authorization with \(name) failed",
                                "\(error.localizedDescription) Check that Cross-Device Handover is on in the Android app, then use the phone's menu → Authorize with Phone….")
                }
            }
        }
    }

    private func authorized(name: String, silent: Bool) {
        Log.write("Authorized with \(name)")
        controller.peers.retryRefused()
        if !silent { alert("Added \(name)", "The phone now accepts this Mac. The link will connect shortly.") }
    }

    private func alert(_ title: String, _ text: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = text
        NSApp.activate(ignoringOtherApps: true)
        alert.runModal()
    }

    @objc private func toggleLaunchAtLogin() {
        do {
            if SMAppService.mainApp.status == .enabled {
                try SMAppService.mainApp.unregister()
            } else {
                try SMAppService.mainApp.register()
            }
            Log.write("Start at login: \(SMAppService.mainApp.status == .enabled ? "on" : "off")")
        } catch {
            Log.write("Start at login failed: \(error.localizedDescription)")
        }
    }

    @objc private func showLog() {
        let url = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Logs/EveryPodsMac.log")
        NSWorkspace.shared.open(url)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
