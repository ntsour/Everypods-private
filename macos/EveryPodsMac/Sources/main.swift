// main.swift
//
// Menu-bar app: shows who holds the AirPods and lets the user take or release them.

import AppKit
import IOBluetooth

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let controller = HandoverController()
    private var authorizer: SetupAuthorizer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem.button?.image = NSImage(systemSymbolName: "airpodspro", accessibilityDescription: "EveryPods")
        if statusItem.button?.image == nil { statusItem.button?.title = "EP" }
        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu
        controller.onChange = { [weak self] in self?.updateIcon() }
        controller.start()
        Log.write("EveryPods for Mac started")
    }

    private func updateIcon() {
        statusItem.button?.appearsDisabled = controller.ownership != .thisMac
    }

    // Rebuilt every time the menu opens, so it always shows current state.
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        menu.addItem(disabled(controller.phoneStatus))
        menu.addItem(disabled(controller.airPodsStatus))
        menu.addItem(disabled(controller.macAudioStatus))
        menu.addItem(.separator())

        let take = NSMenuItem(title: "Take AirPods", action: #selector(takeTapped), keyEquivalent: "")
        take.target = self
        take.isEnabled = controller.pods.isConfigured && !controller.pods.isConnected
        menu.addItem(take)
        let release = NSMenuItem(title: "Release AirPods", action: #selector(releaseTapped), keyEquivalent: "")
        release.target = self
        release.isEnabled = controller.pods.isConnected
        menu.addItem(release)
        menu.addItem(.separator())

        let auto = NSMenuItem(title: "Take AirPods when this Mac plays audio", action: #selector(toggleAutoTake), keyEquivalent: "")
        auto.target = self
        auto.state = Settings.autoTake ? .on : .off
        menu.addItem(auto)
        if !MediaControl.hasPermission {
            let grant = NSMenuItem(title: "Allow Pausing Playback (Accessibility)…", action: #selector(grantAccessibility), keyEquivalent: "")
            grant.target = self
            menu.addItem(grant)
        }
        menu.addItem(.separator())

        menu.addItem(deviceMenu(title: "Phone", current: Settings.phoneAddress, action: #selector(choosePhone(_:))))
        menu.addItem(deviceMenu(title: "AirPods", current: Settings.airPodsAddress, action: #selector(chooseAirPods(_:))))
        let authorize = NSMenuItem(title: "Authorize with Phone…", action: #selector(authorize), keyEquivalent: "")
        authorize.target = self
        authorize.isEnabled = Settings.phoneAddress != nil
        menu.addItem(authorize)
        menu.addItem(.separator())

        let log = NSMenuItem(title: "Show Log", action: #selector(showLog), keyEquivalent: "")
        log.target = self
        menu.addItem(log)
        menu.addItem(NSMenuItem(title: "Quit EveryPods", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
    }

    private func disabled(_ title: String) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.isEnabled = false
        return item
    }

    private func deviceMenu(title: String, current: String?, action: Selector) -> NSMenuItem {
        let parent = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        let submenu = NSMenu()
        for device in Settings.pairedDevices() {
            guard let address = device.addressString else { continue }
            let item = NSMenuItem(title: device.name ?? address, action: action, keyEquivalent: "")
            item.target = self
            item.representedObject = address
            item.state = (current.map(normalizedAddress) == normalizedAddress(address)) ? .on : .off
            submenu.addItem(item)
        }
        if submenu.items.isEmpty { submenu.addItem(disabled("No paired devices")) }
        parent.submenu = submenu
        return parent
    }

    // MARK: Actions

    @objc private func takeTapped() { controller.takeAirPods() }
    @objc private func releaseTapped() { controller.releaseAirPods() }

    @objc private func toggleAutoTake() { Settings.autoTake.toggle() }
    @objc private func grantAccessibility() { MediaControl.requestPermission() }

    @objc private func choosePhone(_ sender: NSMenuItem) {
        Settings.phoneAddress = sender.representedObject as? String
        controller.restartPeer()
    }

    @objc private func chooseAirPods(_ sender: NSMenuItem) {
        Settings.airPodsAddress = sender.representedObject as? String
        controller.onChange?()
    }

    @objc private func authorize() {
        guard let address = Settings.phoneAddress, let authorizer = SetupAuthorizer(phoneAddress: address) else { return }
        self.authorizer = authorizer
        authorizer.run { [weak self] result in
            self?.authorizer = nil
            let alert = NSAlert()
            switch result {
            case .success:
                alert.messageText = "Authorized"
                alert.informativeText = "The phone now accepts this Mac. The link will connect shortly."
                self?.controller.restartPeer()
            case .failure(let error):
                alert.alertStyle = .warning
                alert.messageText = "Authorization failed"
                alert.informativeText = error.localizedDescription
            }
            NSApp.activate(ignoringOtherApps: true)
            alert.runModal()
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
