import XCTest

@testable import BrscanConfigApp
import BrscanConfigCore

/// Unit tests for `ConfigStore` (task 1e.9): missing-file/first-run,
/// load -> edit -> atomic save (preserving a comment and an unknown key),
/// a no-edit round-trip, and dirty-state tracking. Every test drives a
/// `FakeConfigFileSystem`, never the real `~/.config` directory.
final class ConfigStoreTests: XCTestCase {

  private let configURL = URL(fileURLWithPath: "/fake-home/.config/brscan-scand.conf")

  /// A config text exercising every field this app models -- one comment
  /// block, a blank separator line, and one key `DaemonConfig` doesn't
  /// know about (`custom_unknown_key`) -- with every `<dest>.*` value drawn
  /// from `OptionSets` (so `DaemonConfig.from(_:)` reads it back verbatim
  /// rather than substituting a default) and every separation value in its
  /// canonical `serialize(_:)` spelling (`"combine"`, never `"off"`), so
  /// re-applying unedited view models reproduces this text byte-for-byte.
  private var sampleConfigText: String {
    [
      "# Sample config for tests.",
      "# Second comment line.",
      "",
      "printer_host = BRW00AABBCCDDEE.local",
      "display_name = Study Scanner",
      "save_dir = ~/Scans",
      "image_app = Preview",
      "email_to = scans@example.com",
      "custom_unknown_key = keep-me",
      "",
      "file.mode = color",
      "file.source = flatbed",
      "file.dpi = 300",
      "file.format = pdf",
      "file.tiff_compression = lzw",
      "file.separation = combine",
      "file.paper = letter",
      "",
      "image.mode = gray",
      "image.source = adf",
      "image.dpi = 200",
      "image.format = jpeg",
      "image.tiff_compression = g3",
      "image.separation = image:3",
      "image.paper = a4",
      "",
      "ocr.mode = bw",
      "ocr.source = adf-duplex",
      "ocr.dpi = 300",
      "ocr.format = pdf",
      "ocr.tiff_compression = lzw",
      "ocr.separation = page:2",
      "ocr.paper = legal",
      "",
      "email.mode = errdiff",
      "email.source = flatbed",
      "email.dpi = 150",
      "email.format = tiff",
      "email.tiff_compression = g4",
      "email.separation = combine",
      "email.paper = a5",
    ].joined(separator: "\n") + "\n"
  }

  // MARK: Missing file / first run

  func testLoadReportsMissingWhenConfigFileDoesNotExist() throws {
    let fileSystem = FakeConfigFileSystem()
    let store = ConfigStore(fileSystem: fileSystem, configURL: configURL)

    XCTAssertFalse(store.configFileExists)
    try store.load()

    XCTAssertEqual(store.loadState, .missing)
  }

  func testCreateStarterConfigWritesFileAndPopulatesViewModels() throws {
    let fileSystem = FakeConfigFileSystem()
    let store = ConfigStore(fileSystem: fileSystem, configURL: configURL)

    let created = try store.createStarterConfigIfNeeded()

    XCTAssertTrue(created)
    XCTAssertTrue(fileSystem.fileExists(at: configURL))
    XCTAssertEqual(store.loadState, .loaded)

    // Populated from DaemonConfig.default: clean-room, no real device
    // identity (empty printer_host, ~/Scans), not a fabricated hostname.
    XCTAssertEqual(store.generalViewModel.general, DaemonConfig.default.general)
    XCTAssertEqual(store.fileViewModel.route, DaemonConfig.Route.default)
    XCTAssertEqual(store.imageViewModel.route, DaemonConfig.Route.default)
    XCTAssertEqual(store.emailViewModel.route, DaemonConfig.Route.default)
    XCTAssertEqual(store.ocrViewModel.route.mode, DaemonConfig.Route.default.mode)

    // The starter file is itself loadable: parsing it back through
    // ConfigDocument/DaemonConfig produces the same defaults.
    let written = try fileSystem.contents(of: configURL)
    let reloaded = DaemonConfig.from(ConfigDocument(text: written))
    XCTAssertEqual(reloaded, DaemonConfig.default)

    // Freshly created and loaded -- nothing to save yet.
    XCTAssertFalse(store.isDirty)
  }

  func testCreateStarterConfigDoesNotClobberAnExistingFile() throws {
    let fileSystem = FakeConfigFileSystem()
    let existing = "printer_host = BRW00AABBCCDDEE.local\n"
    fileSystem.seedFile(at: configURL, contents: existing)
    let store = ConfigStore(fileSystem: fileSystem, configURL: configURL)

    let created = try store.createStarterConfigIfNeeded()

    XCTAssertFalse(created)
    XCTAssertEqual(try fileSystem.contents(of: configURL), existing, "an existing file must never be overwritten")
    XCTAssertTrue(fileSystem.operations.isEmpty, "create must not touch disk at all when the file already exists")
  }

  // MARK: Load -> edit -> atomic save

  func testSaveWritesAtomicallyPreservingCommentAndUnknownKeyAndClearsDirty() throws {
    let fileSystem = FakeConfigFileSystem()
    fileSystem.seedFile(at: configURL, contents: sampleConfigText)
    let store = ConfigStore(fileSystem: fileSystem, configURL: configURL)
    try store.load()

    XCTAssertFalse(store.isDirty)

    store.generalViewModel.printerHost = "BRWnewhost.local"
    store.fileViewModel.dpi = 600

    XCTAssertTrue(store.isDirty)

    try store.save()

    XCTAssertFalse(store.isDirty)

    let written = try fileSystem.contents(of: configURL)
    XCTAssertTrue(written.contains("# Sample config for tests."), "comment must survive the save")
    XCTAssertTrue(written.contains("custom_unknown_key = keep-me"), "unrecognized key must survive the save")
    XCTAssertTrue(written.contains("printer_host = BRWnewhost.local"), "the edited key must be updated")
    XCTAssertTrue(written.contains("file.dpi = 600"), "the edited key must be updated")
    // Untouched routes keep their original values.
    XCTAssertTrue(written.contains("image.dpi = 200"))
    XCTAssertTrue(written.contains("ocr.separation = page:2"))

    // The save went through a temp file, then a rename -- never a direct
    // write to the target path.
    XCTAssertTrue(
      fileSystem.operations.contains { operation in
        if case .write(let path) = operation { return path != configURL.path }
        return false
      }, "expected a write to a temp path distinct from the target")
    XCTAssertTrue(
      fileSystem.operations.contains { operation in
        if case .move(_, let destination) = operation { return destination == configURL.path }
        return false
      }, "expected a move/rename installing the temp file at the target path")
    guard case .write(let tempPath)? = fileSystem.operations.first else {
      return XCTFail("expected the first operation to be the temp-file write")
    }
    XCTAssertEqual(fileSystem.operations.last, .move(from: tempPath, to: configURL.path))
  }

  func testRoundTripWithNoEditsLeavesBytesStable() throws {
    let fileSystem = FakeConfigFileSystem()
    fileSystem.seedFile(at: configURL, contents: sampleConfigText)
    let store = ConfigStore(fileSystem: fileSystem, configURL: configURL)

    try store.load()
    XCTAssertFalse(store.isDirty)
    try store.save()

    XCTAssertEqual(try fileSystem.contents(of: configURL), sampleConfigText)
  }

  // MARK: Dirty state

  func testDirtyStateTracksLoadEditSaveAndRevert() throws {
    let fileSystem = FakeConfigFileSystem()
    fileSystem.seedFile(at: configURL, contents: sampleConfigText)
    let store = ConfigStore(fileSystem: fileSystem, configURL: configURL)

    try store.load()
    XCTAssertFalse(store.isDirty, "clean immediately after load")

    let originalDpi = store.fileViewModel.dpi
    store.fileViewModel.dpi = originalDpi + 100
    XCTAssertTrue(store.isDirty, "dirty after an edit")

    store.fileViewModel.dpi = originalDpi
    XCTAssertFalse(store.isDirty, "clean again once the field is reverted to its loaded value")

    store.fileViewModel.dpi = originalDpi + 100
    XCTAssertTrue(store.isDirty)
    try store.save()
    XCTAssertFalse(store.isDirty, "clean after a successful save")
  }
}
