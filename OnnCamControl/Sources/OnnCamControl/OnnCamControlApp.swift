import AppKit
import SwiftUI

@main
@MainActor
final class OnnCamControlApp: NSObject, NSApplicationDelegate, NSPopoverDelegate {
    private var model: CameraViewModel!
    private var hostingController: NSHostingController<PanelView>!
    private let popover = NSPopover()
    private var statusItem: NSStatusItem!

    static func main() {
        let app = NSApplication.shared
        let delegate = OnnCamControlApp()
        app.delegate = delegate
        app.setActivationPolicy(.accessory)
        app.run()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        model = CameraViewModel()

        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.button?.image = NSImage(systemSymbolName: "video.badge.waveform", accessibilityDescription: "OnnCam Control")
        statusItem.button?.target = self
        statusItem.button?.action = #selector(togglePanel)
        statusItem.button?.sendAction(on: [.leftMouseUp, .rightMouseUp])

        hostingController = NSHostingController(rootView: PanelView(model: model))
        popover.contentViewController = hostingController
        popover.behavior = .transient
        popover.animates = true
        popover.delegate = self

        Task { await model.refresh() }
    }

    @objc private func togglePanel() {
        guard let button = statusItem.button else { return }
        if popover.isShown {
            popover.performClose(nil)
        } else {
            NSApplication.shared.activate(ignoringOtherApps: true)
            Task { await model.refresh() }

            // Fit the panel to the screen the status item lives on, so the
            // popover (anchored to the menu bar) never overflows off-screen.
            let screen = button.window?.screen ?? NSScreen.main
            let available = (screen?.visibleFrame.height ?? 800) - 40
            let height = min(690, max(420, available))
            hostingController.rootView = PanelView(model: model, panelHeight: height)
            popover.contentSize = NSSize(width: 384, height: height)

            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
        }
    }

    func popoverDidClose(_ notification: Notification) {
        statusItem.button?.highlight(false)
    }
}
