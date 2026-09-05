import AppKit

/// The real `FolderPicking` implementation: opens an `NSOpenPanel` configured
/// to choose exactly one directory. Kept to this one thin method -- no
/// `GeneralViewModel` logic lives here -- so `GeneralViewModel`'s tests can
/// substitute a fake and never launch a panel (see `FolderPicking`).
public struct NSOpenPanelFolderPicker: FolderPicking {
  public init() {}

  public func pickFolder() -> String? {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.allowsMultipleSelection = false
    panel.canCreateDirectories = true
    guard panel.runModal() == .OK, let url = panel.url else { return nil }
    return url.path
  }
}
