import XCTest

@testable import BrscanConfigApp
import BrscanConfigCore

/// A fake `FolderPicking` so these tests never launch a real `NSOpenPanel`.
/// `pathToReturn == nil` simulates the user cancelling the panel.
private final class FakeFolderPicker: FolderPicking {
  var pathToReturn: String?
  private(set) var pickCount = 0

  func pickFolder() -> String? {
    pickCount += 1
    return pathToReturn
  }
}

/// Unit tests for `GeneralViewModel`: binding, the `printer_host`
/// required-field flag, the folder-pick seam (via `FakeFolderPicker`, never
/// a real `NSOpenPanel`), and seeding/round-trip from `DaemonConfig.General`.
/// No UI is launched -- these exercise the view model directly, per task
/// 1e.6's brief.
final class GeneralViewModelTests: XCTestCase {

  // MARK: Binding

  func testSettingEachFieldUpdatesTheProducedGeneral() {
    let viewModel = GeneralViewModel()

    viewModel.printerHost = "BRW00AABBCCDDEE.local"
    viewModel.displayName = "Study Scanner"
    viewModel.saveDir = "~/Scans"
    viewModel.imageApp = "Preview"
    viewModel.emailTo = "scans@example.com"

    let general = viewModel.general
    XCTAssertEqual(general.printerHost, "BRW00AABBCCDDEE.local")
    XCTAssertEqual(general.displayName, "Study Scanner")
    XCTAssertEqual(general.saveDir, "~/Scans")
    XCTAssertEqual(general.imageApp, "Preview")
    XCTAssertEqual(general.emailTo, "scans@example.com")
  }

  // MARK: Required-field flag

  func testEmptyPrinterHostIsMissing() {
    let viewModel = GeneralViewModel()
    viewModel.printerHost = ""
    XCTAssertTrue(viewModel.isPrinterHostMissing)
  }

  func testWhitespaceOnlyPrinterHostIsMissing() {
    let viewModel = GeneralViewModel()
    viewModel.printerHost = "   "
    XCTAssertTrue(viewModel.isPrinterHostMissing)
  }

  func testNonEmptyPrinterHostIsNotMissing() {
    let viewModel = GeneralViewModel()
    viewModel.printerHost = "BRW00AABBCCDDEE.local"
    XCTAssertFalse(viewModel.isPrinterHostMissing)
  }

  // MARK: Folder-pick seam

  func testPickSaveDirUpdatesSaveDirWhenPickerReturnsAPath() {
    let picker = FakeFolderPicker()
    picker.pathToReturn = "/Users/example/Scans"
    let viewModel = GeneralViewModel(folderPicker: picker)

    viewModel.pickSaveDir()

    XCTAssertEqual(viewModel.saveDir, "/Users/example/Scans")
    XCTAssertEqual(picker.pickCount, 1)
  }

  func testPickSaveDirLeavesSaveDirUnchangedWhenPickerCancels() {
    let picker = FakeFolderPicker()
    picker.pathToReturn = nil
    let viewModel = GeneralViewModel(
      general: DaemonConfig.General(
        printerHost: "", displayName: "", saveDir: "~/Scans", imageApp: "", emailTo: ""),
      folderPicker: picker)

    viewModel.pickSaveDir()

    XCTAssertEqual(viewModel.saveDir, "~/Scans")
    XCTAssertEqual(picker.pickCount, 1)
  }

  // MARK: Seeding / round-trip

  func testSeedingFromGeneralPopulatesFields() {
    let seed = DaemonConfig.General(
      printerHost: "BRW00AABBCCDDEE.local",
      displayName: "Study Scanner",
      saveDir: "~/Scans/Inbox",
      imageApp: "Preview",
      emailTo: "scans@example.com")

    let viewModel = GeneralViewModel(general: seed)

    XCTAssertEqual(viewModel.printerHost, seed.printerHost)
    XCTAssertEqual(viewModel.displayName, seed.displayName)
    XCTAssertEqual(viewModel.saveDir, seed.saveDir)
    XCTAssertEqual(viewModel.imageApp, seed.imageApp)
    XCTAssertEqual(viewModel.emailTo, seed.emailTo)
  }

  func testSeedingThenProducingGeneralRoundTrips() {
    let seed = DaemonConfig.General(
      printerHost: "BRW00AABBCCDDEE.local",
      displayName: "Study Scanner",
      saveDir: "~/Scans/Inbox",
      imageApp: "Preview",
      emailTo: "scans@example.com")

    let viewModel = GeneralViewModel(general: seed)

    XCTAssertEqual(viewModel.general, seed)
  }

  func testDefaultInitSeedsFromDaemonConfigDefault() {
    let viewModel = GeneralViewModel()
    XCTAssertEqual(viewModel.general, DaemonConfig.default.general)
  }
}
