// StayOpenMenuItem.swift
//
// A checkable menu item that does not close the menu when clicked, so several settings can be changed in one go.
// AppKit closes a menu on any click on a normal item; a click on an item with a custom view is left to the view.
// The view draws like a standard item (checkmark, title, hover highlight). Keyboard navigation skips such items.

import AppKit

final class StayOpenItemView: NSView {
    private let title: String
    private let isOn: () -> Bool
    private let enabled: Bool
    private let onClick: () -> Void
    /// Called after a click so sibling items (and the parent's title) can show the new state.
    var afterClick: (() -> Void)?
    private var hovering = false
    private let font = NSFont.menuFont(ofSize: 0)
    private static let leftInset: CGFloat = 22, rightInset: CGFloat = 16, height: CGFloat = 22

    init(title: String, enabled: Bool = true, isOn: @escaping () -> Bool, onClick: @escaping () -> Void) {
        self.title = title
        self.isOn = isOn
        self.enabled = enabled
        self.onClick = onClick
        let width = ceil((title as NSString).size(withAttributes: [.font: font]).width) + Self.leftInset + Self.rightInset
        super.init(frame: NSRect(x: 0, y: 0, width: width, height: Self.height))
        autoresizingMask = .width
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect],
                                       owner: self, userInfo: nil))
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        hovering = false
    }

    override func mouseEntered(with event: NSEvent) { hovering = true; needsDisplay = true }
    override func mouseExited(with event: NSEvent) { hovering = false; needsDisplay = true }

    override func mouseUp(with event: NSEvent) {
        guard enabled else { return }
        onClick()
        afterClick?()
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        let highlighted = hovering && enabled
        if highlighted {
            NSColor.selectedContentBackgroundColor.setFill()
            NSBezierPath(roundedRect: bounds.insetBy(dx: 5, dy: 1), xRadius: 4, yRadius: 4).fill()
        }
        let color: NSColor = !enabled ? .disabledControlTextColor : highlighted ? .selectedMenuItemTextColor : .labelColor
        let attributes: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color]
        let textHeight = (title as NSString).size(withAttributes: attributes).height
        let y = (bounds.height - textHeight) / 2
        if isOn() { ("✓" as NSString).draw(at: NSPoint(x: 8, y: y), withAttributes: attributes) }
        (title as NSString).draw(at: NSPoint(x: Self.leftInset, y: y), withAttributes: attributes)
    }

    /// Makes a menu item backed by this view. After a click, every stay-open item in the same menu redraws.
    static func item(_ title: String, enabled: Bool = true, isOn: @escaping () -> Bool,
                     onClick: @escaping () -> Void, afterClick: (() -> Void)? = nil) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        let view = StayOpenItemView(title: title, enabled: enabled, isOn: isOn, onClick: onClick)
        view.afterClick = { [weak item] in
            item?.menu?.items.forEach { $0.view?.needsDisplay = true }
            afterClick?()
        }
        item.view = view
        return item
    }
}
