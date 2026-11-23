// HoldPanel.swift
//
// Floating, non-activating progress panel for the playback hold / call handover. Shown top-right under the
// menu bar like the system AirPods HUD. Driven entirely by PlaybackHoldActions.onStatusChange.
// Everything runs on the main thread.

import AppKit

private final class HoldNSPanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

final class HoldPanel {
    private let hold: PlaybackHoldActions
    private let panel: NSPanel
    private let icon = NSImageView()
    private let titleLabel = NSTextField(labelWithString: "")
    private let statusLabel = NSTextField(labelWithString: "")
    private let progress = NSProgressIndicator()
    private let retryButton = NSButton(title: "Retry", target: nil, action: nil)
    private let playHereButton = NSButton(title: "Play on Mac", target: nil, action: nil)
    private let closeButton = NSButton(title: "Close", target: nil, action: nil)
    private let buttonRow = NSStackView()

    private var status: HoldStatus?
    private var timer: Timer?
    private var hideWork: DispatchWorkItem?
    private var visible = false

    private static let width: CGFloat = 320

    init(hold: PlaybackHoldActions) {
        self.hold = hold
        panel = HoldNSPanel(contentRect: NSRect(x: 0, y: 0, width: Self.width, height: 80),
                            styleMask: [.nonactivatingPanel, .borderless],
                            backing: .buffered, defer: true)
        panel.level = .statusBar
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
        panel.hidesOnDeactivate = false
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.isMovableByWindowBackground = false
        panel.becomesKeyOnlyIfNeeded = true
        panel.alphaValue = 0
        buildContent()

        hold.onStatusChange = { [weak self] status in self?.apply(status) }
        if let current = hold.status { apply(current) }
    }

    deinit { timer?.invalidate() }

    // MARK: - Layout

    private func buildContent() {
        let effect = NSVisualEffectView()
        effect.material = .hudWindow
        effect.state = .active
        effect.blendingMode = .behindWindow
        effect.wantsLayer = true
        effect.layer?.cornerRadius = 14
        effect.layer?.masksToBounds = true
        effect.translatesAutoresizingMaskIntoConstraints = false

        let image = NSImage(systemSymbolName: "airpodspro", accessibilityDescription: nil)
            ?? NSImage(systemSymbolName: "headphones", accessibilityDescription: nil)
        icon.image = image
        icon.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 26, weight: .regular)
        icon.contentTintColor = .labelColor
        icon.translatesAutoresizingMaskIntoConstraints = false
        icon.setAccessibilityElement(false)

        titleLabel.font = .systemFont(ofSize: 13, weight: .semibold)
        titleLabel.textColor = .labelColor
        titleLabel.lineBreakMode = .byTruncatingTail
        statusLabel.font = .systemFont(ofSize: 12)
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.lineBreakMode = .byTruncatingTail
        statusLabel.setAccessibilityElement(true)
        statusLabel.setAccessibilityRole(.staticText)

        progress.style = .bar
        progress.isIndeterminate = false
        progress.minValue = 0
        progress.maxValue = 1
        progress.controlSize = .small
        progress.translatesAutoresizingMaskIntoConstraints = false

        retryButton.bezelStyle = .rounded
        retryButton.controlSize = .small
        retryButton.target = self
        retryButton.action = #selector(retryTapped)
        playHereButton.bezelStyle = .rounded
        playHereButton.controlSize = .small
        playHereButton.target = self
        playHereButton.action = #selector(playHereTapped)

        buttonRow.orientation = .horizontal
        buttonRow.spacing = 8
        buttonRow.addArrangedSubview(retryButton)
        buttonRow.addArrangedSubview(playHereButton)

        closeButton.bezelStyle = .regularSquare
        closeButton.isBordered = false
        closeButton.image = NSImage(systemSymbolName: "xmark", accessibilityDescription: "Close")
        closeButton.imagePosition = .imageOnly
        closeButton.contentTintColor = .secondaryLabelColor
        closeButton.target = self
        closeButton.action = #selector(closeTapped)
        closeButton.setAccessibilityLabel("Close")
        closeButton.translatesAutoresizingMaskIntoConstraints = false

        let textStack = NSStackView(views: [titleLabel, statusLabel])
        textStack.orientation = .vertical
        textStack.alignment = .leading
        textStack.spacing = 1

        let top = NSStackView(views: [icon, textStack])
        top.orientation = .horizontal
        top.alignment = .centerY
        top.spacing = 10

        let column = NSStackView(views: [top, progress, buttonRow])
        column.orientation = .vertical
        column.alignment = .leading
        column.spacing = 8
        column.translatesAutoresizingMaskIntoConstraints = false

        effect.addSubview(column)
        effect.addSubview(closeButton)
        NSLayoutConstraint.activate([
            column.leadingAnchor.constraint(equalTo: effect.leadingAnchor, constant: 14),
            column.trailingAnchor.constraint(equalTo: effect.trailingAnchor, constant: -14),
            column.topAnchor.constraint(equalTo: effect.topAnchor, constant: 12),
            column.bottomAnchor.constraint(equalTo: effect.bottomAnchor, constant: -12),
            progress.widthAnchor.constraint(equalTo: column.widthAnchor),
            icon.widthAnchor.constraint(equalToConstant: 32),
            effect.widthAnchor.constraint(equalToConstant: Self.width),
            closeButton.topAnchor.constraint(equalTo: effect.topAnchor, constant: 8),
            closeButton.trailingAnchor.constraint(equalTo: effect.trailingAnchor, constant: -10),
            closeButton.widthAnchor.constraint(equalToConstant: 16),
            closeButton.heightAnchor.constraint(equalToConstant: 16),
            textStack.trailingAnchor.constraint(lessThanOrEqualTo: column.trailingAnchor, constant: -18),
        ])
        panel.contentView = effect
    }

    private func reposition() {
        guard let effect = panel.contentView, let screen = NSScreen.screens.first else { return }
        effect.layoutSubtreeIfNeeded()
        let size = NSSize(width: Self.width, height: effect.fittingSize.height)
        let frame = screen.frame
        let visibleFrame = screen.visibleFrame   // excludes the menu bar
        // Centred under the menu bar: the top right is where macOS shows its own AirPods connection banner.
        let x = frame.midX - size.width / 2
        let y = visibleFrame.maxY - size.height - 6
        panel.setFrame(NSRect(origin: NSPoint(x: x, y: y), size: size), display: true)
    }

    // MARK: - Status

    private func apply(_ new: HoldStatus?) {
        guard let s = new else {
            status = nil
            stopTicking()
            // A "connected" confirmation fades out on its own timer.
            if hideWork == nil { fadeOut() }
            return
        }
        let previous = status
        status = s
        hideWork?.cancel()
        hideWork = nil
        render(s)
        if !visible || previous == nil { fadeIn() } else { reposition() }

        switch s.phase {
        case .connected:
            stopTicking()
            let work = DispatchWorkItem { [weak self] in
                guard let self = self else { return }
                self.hideWork = nil
                self.fadeOut()
            }
            hideWork = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5, execute: work)
        case .failed:
            stopTicking()
        default:
            startTicking()
        }
    }

    private func render(_ s: HoldStatus) {
        let name = s.deviceName
        titleLabel.stringValue = name
        let isCall = s.kind == .call
        let text: String
        switch s.phase {
        case .requestingRelease: text = "Asking your phone to let go…"
        case .connecting: text = isCall ? "Moving your call to \(name)…" : "Connecting to \(name)…"
        case .routing: text = isCall ? "Switching call audio…" : "Switching audio…"
        case .connected: text = "Connected"
        case .failed: text = "Couldn't connect to \(name)"
        }
        statusLabel.stringValue = text
        panel.setAccessibilityLabel(text)
        statusLabel.setAccessibilityLabel(text)

        let failed = s.phase == .failed
        progress.isHidden = failed
        buttonRow.isHidden = !failed
        closeButton.isHidden = !failed
        playHereButton.title = isCall ? "Use Mac" : "Play on Mac"
        if !failed { progress.doubleValue = s.progress(at: Date()) }
        reposition()
    }

    private func startTicking() {
        guard timer == nil else { return }
        let t = Timer(timeInterval: 0.1, repeats: true) { [weak self] _ in
            guard let self = self, let s = self.status else { return }
            self.progress.doubleValue = s.progress(at: Date())
        }
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    private func stopTicking() {
        timer?.invalidate()
        timer = nil
    }

    // MARK: - Fading

    private func fadeIn() {
        visible = true
        reposition()
        panel.orderFrontRegardless()
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = 0.2
            panel.animator().alphaValue = 1
        }
    }

    private func fadeOut() {
        guard visible else { return }
        visible = false
        NSAnimationContext.runAnimationGroup({ [weak self] ctx in
            ctx.duration = 0.2
            self?.panel.animator().alphaValue = 0
        }, completionHandler: { [weak self] in
            guard let self = self, !self.visible else { return }
            self.panel.orderOut(nil)
        })
    }

    // MARK: - Actions

    @objc private func retryTapped() { hold.retry() }
    @objc private func playHereTapped() { hold.playHere() }
    @objc private func closeTapped() { hold.dismissStayPaused() }
}
