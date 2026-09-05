import AppKit
import SwiftUI

/// Placeholder `MenuBarExtra` content: a static daemon-status stub and a
/// "Preferences..." item that opens the config window.
///
/// Task 1e.5 doesn't wire either to real state -- the status line is static
/// text (real daemon polling comes later), and "Preferences..." only opens
/// the existing `WindowGroup` via `openWindow`.
struct MenuBarContentView: View {
  @Environment(\.openWindow) private var openWindow

  var body: some View {
    // Static placeholder; real daemon-status polling is a later task.
    Text("Daemon status: unknown")
      .foregroundStyle(.secondary)

    Divider()

    Button("Preferences…") {
      openWindow(id: BrscanConfigApp.mainWindowID)
    }

    Divider()

    Button("Quit") {
      NSApplication.shared.terminate(nil)
    }
  }
}
