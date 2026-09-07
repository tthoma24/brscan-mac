import AppKit
import SwiftUI

/// `MenuBarExtra` content: a real daemon-status line (task 1e.10) plus a
/// "Preferences..." item that opens the config window.
///
/// Task 1e.5 left the status line a static stub; this task wires it to a
/// `DaemonViewModel`, refreshed whenever the menu opens (`onAppear`, which
/// SwiftUI fires each time this content becomes visible again) rather than
/// on a timer, since a menu-bar item's content is only worth re-querying
/// while someone is actually looking at it.
struct MenuBarContentView: View {
  @Environment(\.openWindow) private var openWindow
  @StateObject private var daemon = DaemonViewModel()

  var body: some View {
    Text("Daemon status: \(daemon.state.statusText)")
      .foregroundStyle(.secondary)
      .onAppear {
        daemon.refreshState()
      }

    Button("Refresh Status") {
      daemon.refreshState()
    }

    Divider()

    Button("Preferences…") {
      // Pair the open with app activation: under LSUIElement the window can
      // otherwise surface behind the frontmost app (see BrscanConfigApp).
      BrscanConfigApp.openConfigWindow(openWindow)
    }

    Divider()

    Button("Quit") {
      NSApplication.shared.terminate(nil)
    }
  }
}
