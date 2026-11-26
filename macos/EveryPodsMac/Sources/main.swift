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
    /// Floating handover-progress panel, driven by the controller's playback hold.
    private var holdPanel: HoldPanel?
    private var gattAuthorizer: GattSetupAuthorizer?
    private var menuCandidates: [PeerRegistry.Candidate] = []

    func applicationWillTerminate(_ notification: Notification) {
        Log.writeNow("Exit: EveryPods quit")
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        Log.installExitLogging()
        // Single instance: if another copy is already running, quit silently.
        if let id = Bundle.main.bundleIdentifier,
           NSRunningApplication.runningApplications(withBundleIdentifier: id).contains(where: { $0 != .current }) {
            Log.writeNow("Exit: another copy of EveryPods is already running")
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
        holdPanel = HoldPanel(hold: controller.hold)
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

        menu.addItem(stayOpenToggle("Take over when media starts", { Settings.takeOnMediaStart }) { [weak self] in self?.toggleTakeMedia() })
        menu.addItem(stayOpenToggle("Take over when receiving a call", { Settings.takeOnCall }) { [weak self] in self?.toggleTakeCall() })
        menu.addItem(stayOpenToggle("Auto-set AirPods as audio device", { Settings.autoRoute }) { [weak self] in self?.toggleAutoRoute() })
        menu.addItem(stayOpenToggle("Pause playback when handing over", { Settings.pauseOnHandover }) { [weak self] in self?.togglePause() })
        menu.addItem(stayOpenToggle("Hold audio until AirPods connect", { Settings.holdPlayback }) { [weak self] in self?.toggleHold() })
        menu.addItem(triggerAppsMenu())
        menu.addItem(stayOpenToggle("Connect AirPods when the case opens (if this Mac used them last)",
                                    { Settings.lidOpenConnect }) { [weak self] in self?.toggleLidOpen() })
        if !MediaControl.hasPermission {
            menu.addItem(action("Allow Pausing Playback (Accessibility)…", #selector(grantAccessibility)))
        }
        menu.addItem(.separator())

        menu.addItem(stayOpenToggle("Start at login", { SMAppService.mainApp.status == .enabled }) { [weak self] in self?.toggleLaunchAtLogin() })
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

    /// A setting toggle that keeps the menu open when clicked (StayOpenItemView).
    private func stayOpenToggle(_ title: String, _ isOn: @escaping () -> Bool, _ onClick: @escaping () -> Void) -> NSMenuItem {
        StayOpenItemView.item(title, isOn: isOn, onClick: onClick)
    }

    /// "Apps that take or keep the AirPods" (same settings as the Windows tray menu): the built-in apps, every app
    /// that used sound, and any the user set, sorted by name. Every app has the same three settings; built-in
    /// apps start pre-configured and offer "Reset to built-in" once changed. Clicks keep the menu open.
    private func triggerAppsMenu() -> NSMenuItem {
        let parent = NSMenuItem(title: "Apps that take or keep the AirPods", action: nil, keyEquivalent: "")
        let submenu = NSMenu()
        let rules = Settings.triggerApps
        var keys: [String] = []
        func add(_ bundle: String) {
            let k = rules.key(bundle)
            if !k.isEmpty, k != "?", !keys.contains(k) { keys.append(k) }
        }
        TriggerAppList.builtIn.map(\.bundle).forEach(add)
        rules.user.keys.forEach(add)
        controller.media.seenApps.reversed().prefix(40).forEach(add)
        let apps = keys.map { k in (key: k, name: TriggerAppList.builtIn(k)?.name ?? UserIntentWatcher.displayName(k)) }
            .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        for (key, name) in apps {
            let label: () -> String = {
                let now = Settings.triggerApps
                let tag = TriggerAppList.builtIn(key) == nil ? "" : now.isChangedBuiltIn(key) ? " (built in, changed)" : " (built in)"
                return "\(name)\(tag): \(now.settings(key).text)"
            }
            let item = NSMenuItem(title: label(), action: nil, keyEquivalent: "")
            let choices = NSMenu()
            choices.autoenablesItems = false
            let relabel: () -> Void = { [weak item] in item?.title = label() }
            for setting in TriggerSetting.allCases {
                choices.addItem(StayOpenItemView.item(setting.title,
                                                      isOn: { setting.get(Settings.triggerApps.settings(key)) },
                                                      onClick: { [weak self] in self?.toggleSetting(key, setting) },
                                                      afterClick: relabel))
            }
            if TriggerAppList.builtIn(key) != nil {
                choices.addItem(.separator())
                // Greyed (nothing to reset) until the app is changed.
                choices.addItem(StayOpenItemView.item("Reset to built-in", enabled: { Settings.triggerApps.isChangedBuiltIn(key) },
                                                      isOn: { false },
                                                      onClick: { [weak self] in self?.resetTrigger(key) },
                                                      afterClick: relabel))
            }
            item.submenu = choices
            submenu.addItem(item)
        }
        parent.submenu = submenu
        return parent
    }

    private func toggleSetting(_ key: String, _ setting: TriggerSetting) {
        var rules = Settings.triggerApps
        guard rules.toggle(key, setting) else { return }
        Settings.triggerApps = rules
        Log.write("Trigger apps: \(key): \(rules.settings(key).text)")
    }

    private func resetTrigger(_ key: String) {
        var rules = Settings.triggerApps
        guard rules.reset(key) else { return }
        Settings.triggerApps = rules
        Log.write("Trigger apps: \(key) reset to built-in (\(rules.settings(key).text))")
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
    @objc private func toggleHold() {
        Settings.holdPlayback.toggle()
        // Pausing needs the Accessibility permission (the speakers are muted either way).
        if Settings.holdPlayback, !MediaControl.hasPermission { MediaControl.requestPermission() }
    }
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
