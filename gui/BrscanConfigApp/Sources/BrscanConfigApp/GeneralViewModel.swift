import BrscanConfigCore
import Foundation

/// A seam for choosing a folder, so `GeneralViewModel` can be unit-tested
/// without ever launching a real `NSOpenPanel`. `NSOpenPanelFolderPicker`
/// (below) is the app's real implementation; tests inject a fake that
/// returns a canned path or `nil` (cancel) with no UI involved.
public protocol FolderPicking {
  /// Returns the chosen absolute path, or `nil` if the user cancelled.
  func pickFolder() -> String?
}

/// View model for the **General** tab: wraps `DaemonConfig.General` and
/// exposes `@Published`, individually bindable properties for its five
/// fields (`printerHost`, `displayName`, `saveDir`, `imageApp`, `emailTo`),
/// plus `isPrinterHostMissing`, the required-field flag the daemon itself
/// enforces (see `DaemonConfig.General.printerHost`'s doc comment: an empty
/// `printer_host` has no safe default and the daemon refuses to start).
///
/// This establishes the view-model + binding pattern task 1e.7's reusable
/// route editor (a `RouteViewModel` over `DaemonConfig.Route`) follows: one
/// `ObservableObject` per config section, seeded from the typed
/// `BrscanConfigCore` struct, exposing per-field `@Published` properties
/// plus a computed property that reassembles the struct for a later save
/// (task 1e.9) -- never a single app-wide view model that owns every field
/// directly, so each tab's view model stays small and independently
/// testable.
///
/// No file load/save here (task 1e.9): the view model is always seeded from
/// an in-memory `DaemonConfig.General`, defaulting to
/// `DaemonConfig.default.general`.
public final class GeneralViewModel: ObservableObject {
  @Published public var printerHost: String
  @Published public var displayName: String
  @Published public var saveDir: String
  @Published public var imageApp: String
  @Published public var emailTo: String

  private let folderPicker: FolderPicking

  public init(
    general: DaemonConfig.General = DaemonConfig.default.general,
    folderPicker: FolderPicking = NSOpenPanelFolderPicker()
  ) {
    self.printerHost = general.printerHost
    self.displayName = general.displayName
    self.saveDir = general.saveDir
    self.imageApp = general.imageApp
    self.emailTo = general.emailTo
    self.folderPicker = folderPicker
  }

  /// `true` when `printerHost` is empty or all whitespace -- the daemon's
  /// own "not configured" condition (see `DaemonConfig.General.printerHost`).
  /// The view uses this to show a validation affordance; it does not block
  /// editing or saving (`docs/PLAN-1E-DESIGN.md`'s "Required field": a
  /// partial config is valid on disk, it just won't start the daemon yet).
  public var isPrinterHostMissing: Bool {
    printerHost.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
  }

  /// Reassembles the five bound fields into a `DaemonConfig.General`,
  /// reflecting whatever edits have been made. A later save (task 1e.9)
  /// serializes this via `DaemonConfig.General.apply(to:)`.
  public var general: DaemonConfig.General {
    DaemonConfig.General(
      printerHost: printerHost, displayName: displayName, saveDir: saveDir, imageApp: imageApp, emailTo: emailTo)
  }

  /// Invokes the injected `folderPicker` and, if it returns a path (the user
  /// didn't cancel), updates `saveDir`. Kept as one thin method so the
  /// SwiftUI button handler has nothing else to do, and so a test can drive
  /// the same seam with a fake picker.
  public func pickSaveDir() {
    guard let path = folderPicker.pickFolder() else { return }
    saveDir = path
  }
}
