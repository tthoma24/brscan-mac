import AppKit
import SwiftUI

import BrscanConfigCore

/// The config app: a menu-bar agent (`MenuBarExtra`) plus a tabbed,
/// preferences-style window for the daemon's per-destination defaults.
///
/// The app ships as an ad-hoc-signed `.app` bundle with `LSUIElement = true`
/// (see `Info.plist.in` and the CMake `brscan-config-app` target), so it runs
/// as a menu-bar-only agent with no Dock icon. `LSUIElement` does not hide the
/// `WindowGroup`, but it can let the window open *behind* the frontmost app, so
/// every path that opens the window (`openConfigWindow`, called at launch and
/// from the menu's "Preferences…") pairs `openWindow(id:)` with
/// `NSApplication.shared.activate(ignoringOtherApps:)` to bring it forward.
@main
struct BrscanConfigApp: App {
  /// Identifier for the main config window, so `MenuBarContentView`'s
  /// "Preferences…" item can target it with `openWindow(id:)`.
  static let mainWindowID = "main"

  /// Opens the config window and brings the app forward. Because
  /// `LSUIElement` makes this a background agent, `openWindow(id:)` alone can
  /// surface the window behind the current frontmost app; the explicit
  /// `activate(ignoringOtherApps:)` guarantees it comes to the front.
  static func openConfigWindow(_ openWindow: OpenWindowAction) {
    openWindow(id: mainWindowID)
    NSApplication.shared.activate(ignoringOtherApps: true)
  }

  @Environment(\.openWindow) private var openWindow

  var body: some Scene {
    WindowGroup("Brscan Config", id: Self.mainWindowID) {
      ContentView()
        .onAppear {
          // As a menu-bar agent the app has no Dock icon to click, so make
          // sure the launch-time window is the frontmost window.
          NSApplication.shared.activate(ignoringOtherApps: true)
        }
    }

    MenuBarExtra("Brscan Config", systemImage: "printer") {
      MenuBarContentView()
    }
  }
}
