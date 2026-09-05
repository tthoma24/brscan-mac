import SwiftUI

// Importing `BrscanConfigCore` here confirms the local-package dependency
// resolves; the shell doesn't bind any UI to it yet (Tasks 1e.6-1e.8).
import BrscanConfigCore

/// The Plan 1e config app shell: a menu-bar item plus a five-tab
/// preferences-style window (Task 1e.5 -- static placeholders only, no real
/// bindings).
///
/// Deferred to a later task: a signable `.app` bundle, `Info.plist` /
/// `LSUIElement` (to run as a menu-bar-only background app), code signing,
/// and an actually-functioning `MenuBarExtra` -- this target is a SwiftPM
/// executable, which can produce and run a binary but not a signed `.app`
/// bundle, so none of that is exercised yet. The acceptance bar here is that
/// this shell compiles with `swift build`.
@main
struct BrscanConfigApp: App {
  /// Identifier for the main config window, so `MenuBarContentView`'s
  /// "Preferences…" item can target it with `openWindow(id:)`.
  static let mainWindowID = "main"

  var body: some Scene {
    WindowGroup("Brscan Config", id: Self.mainWindowID) {
      ContentView()
    }

    MenuBarExtra("Brscan Config", systemImage: "printer") {
      MenuBarContentView()
    }
  }
}
