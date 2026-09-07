import XCTest

@testable import BrscanConfigCore

/// Tests for `DaemonConfig` (Task 1e.4): the typed model on top of
/// `ConfigDocument` (Task 1e.2) and the option-set vocabulary (Task 1e.3).
/// Every fixture uses synthetic values only -- `BRW00AABBCCDDEE` is a
/// placeholder, never a real device identity.
final class DaemonConfigTests: XCTestCase {

  // MARK: Fixture loading

  private func fixtureURL(_ name: String) throws -> URL {
    try XCTUnwrap(
      Bundle.module.url(forResource: name, withExtension: "conf", subdirectory: "Fixtures"),
      "Missing Fixtures/\(name).conf")
  }

  private func fixtureText(_ name: String) throws -> String {
    try String(contentsOf: fixtureURL(name), encoding: .utf8)
  }

  // MARK: Parse -> model, with expected typed values

  /// Parsing a document with a fully-specified `file` route yields exactly
  /// those typed values.
  func testParseFullySpecifiedRouteYieldsExpectedTypedValues() throws {
    let doc = ConfigDocument(text: try fixtureText("daemon-config-sample"))
    let config = DaemonConfig.from(doc)

    XCTAssertEqual(config.general.printerHost, "BRW00AABBCCDDEE.local")
    XCTAssertEqual(config.general.displayName, "Test Mac")
    XCTAssertEqual(config.general.saveDir, "~/Scans")
    XCTAssertEqual(config.general.imageApp, "Preview")
    XCTAssertEqual(config.general.emailTo, "someone@example.com")

    XCTAssertEqual(config.file.mode, "color")
    XCTAssertEqual(config.file.dpi, 300)
    XCTAssertEqual(config.file.source, "flatbed")
    XCTAssertEqual(config.file.format, "pdf")
    XCTAssertEqual(config.file.tiffCompression, "lzw")
    XCTAssertEqual(config.file.separation, .combine)
    XCTAssertEqual(config.file.paper, "LETTER")
  }

  /// Serializing an unchanged, fully-round-tripped model back out preserves
  /// every owned key's value, plus unknown keys and comments.
  func testParseThenApplyKeepsOwnedKeysCorrectAndUnknownKeysAndCommentsSurvive() throws {
    let original = try fixtureText("daemon-config-sample")
    var doc = ConfigDocument(text: original)
    let config = DaemonConfig.from(doc)

    config.apply(to: &doc)
    let serialized = doc.serialized()

    // Owned keys read back correctly.
    XCTAssertEqual(doc.value(for: "file.paper"), "LETTER")
    XCTAssertEqual(doc.value(for: "ocr.separation"), "page:2")
    XCTAssertEqual(doc.value(for: "email.mode"), "truegray")

    // Unknown key and comments untouched.
    XCTAssertEqual(doc.value(for: "some.unknown"), "key")
    XCTAssertTrue(serialized.contains("# a key this editor doesn't know about, must survive untouched"))
    XCTAssertTrue(serialized.contains("# --- file: fully specified ---------------------------------------------"))
  }

  // MARK: Byte-stable round trip

  /// `apply` of an unchanged `DaemonConfig`, parsed from a document where
  /// every owned key is already present and spelled exactly as
  /// `ConfigDocument.setValue` would write it, leaves the document
  /// byte-for-byte identical.
  func testApplyOfUnchangedModelIsByteStableWhenEveryOwnedKeyIsAlreadyPresent() throws {
    let original = try fixtureText("daemon-config-full")
    var doc = ConfigDocument(text: original)
    let config = DaemonConfig.from(doc)

    config.apply(to: &doc)

    XCTAssertEqual(doc.serialized(), original)
  }

  // MARK: Default fill-in

  /// A document missing a `<dest>.*` key yields the daemon's default for
  /// it -- checked here against the `image`/`ocr`/`email` routes in the
  /// partial fixture, which each omit different keys.
  func testMissingDestKeysFillInDaemonDefaults() throws {
    let doc = ConfigDocument(text: try fixtureText("daemon-config-sample"))
    let config = DaemonConfig.from(doc)

    // image: format/tiff_compression/separation/paper all omitted.
    XCTAssertEqual(config.image.format, "native")
    XCTAssertEqual(config.image.tiffCompression, "lzw")
    XCTAssertEqual(config.image.separation, .combine)
    XCTAssertEqual(config.image.paper, "")

    // ocr: dpi/format/tiff_compression/paper omitted.
    XCTAssertEqual(config.ocr.dpi, 300)
    XCTAssertEqual(config.ocr.format, "native")
    XCTAssertEqual(config.ocr.tiffCompression, "lzw")
    XCTAssertEqual(config.ocr.paper, "")

    // email: source/separation/paper omitted.
    XCTAssertEqual(config.email.source, "flatbed")
    XCTAssertEqual(config.email.separation, .combine)
    XCTAssertEqual(config.email.paper, "")
  }

  /// A document missing every general key entirely falls back to
  /// `DaemonConfig.default`'s general values.
  func testMissingGeneralKeysFillInDefaults() {
    let doc = ConfigDocument(text: "file.mode = color\n")
    let config = DaemonConfig.from(doc)

    XCTAssertEqual(config.general, DaemonConfig.default.general)
  }

  // MARK: Empty display_name is left absent, not written blank (Review I1)

  /// Applying a `General` whose `displayName` is `""` -- `DaemonConfig
  /// .default`'s value, and what a user leaves an untouched General tab's
  /// field at -- must not emit a literal `display_name = ` line. The
  /// daemon's own hostname-derived default (`DefaultDisplayName()`) only
  /// applies when the key is missing entirely; a present-but-empty value
  /// overrides it with a blank Scan-menu name (see `daemon/config.cpp`'s
  /// `ApplyKey`/`DefaultConfig`).
  func testApplyOfEmptyDisplayNameOmitsTheKeyEntirely() {
    var doc = ConfigDocument()
    DaemonConfig.default.apply(to: &doc)

    XCTAssertNil(doc.value(for: "display_name"))
    XCTAssertFalse(doc.serialized().contains("display_name"), "must not write display_name at all when empty")
  }

  /// The same "don't write it blank" rule applies when the key already had
  /// a value on disk and the user clears the field back to empty: the key
  /// must end up fully absent (falling back to the daemon's hostname
  /// default), not present with an empty value.
  func testApplyOfClearedDisplayNameRemovesAPreviouslyPresentKey() throws {
    var doc = ConfigDocument(text: try fixtureText("daemon-config-sample"))
    XCTAssertEqual(doc.value(for: "display_name"), "Test Mac")

    var config = DaemonConfig.from(doc)
    config.general.displayName = ""
    config.apply(to: &doc)

    XCTAssertNil(doc.value(for: "display_name"))
    XCTAssertFalse(doc.serialized().contains("display_name"))
  }

  /// A non-empty `displayName` is still written normally -- this fix must
  /// not turn `apply` into a no-op for the common, filled-in case.
  func testApplyOfNonEmptyDisplayNameWritesItNormally() {
    var doc = ConfigDocument()
    var config = DaemonConfig.default
    config.general.displayName = "Study Scanner"
    config.apply(to: &doc)

    XCTAssertEqual(doc.value(for: "display_name"), "Study Scanner")
  }

  // MARK: Unknown-key passthrough

  /// A document with an extra, unrecognized key still has it, unchanged,
  /// after a full parse -> apply round trip.
  func testUnknownKeyPassesThroughApply() throws {
    var doc = ConfigDocument(text: try fixtureText("daemon-config-sample"))
    let config = DaemonConfig.from(doc)

    config.apply(to: &doc)

    XCTAssertEqual(doc.value(for: "some.unknown"), "key")
  }

  // MARK: Malformed value -> default

  /// `<dest>.dpi` set to a non-numeric value takes the default dpi, the
  /// same way `daemon/config.cpp`'s `ParsePositiveInt` rejecting it leaves
  /// the field at its prior/default value.
  func testMalformedDpiFallsBackToDefault() throws {
    let doc = ConfigDocument(text: try fixtureText("daemon-config-sample"))
    let config = DaemonConfig.from(doc)

    // email.dpi = abc in the fixture.
    XCTAssertEqual(config.email.dpi, DaemonConfig.Route.default.dpi)
    XCTAssertEqual(config.email.dpi, 300)
  }

  func testMalformedModeFallsBackToDefault() {
    let doc = ConfigDocument(text: "file.mode = not-a-mode\n")
    let config = DaemonConfig.from(doc)
    XCTAssertEqual(config.file.mode, DaemonConfig.Route.default.mode)
  }

  func testMalformedSourceFallsBackToDefault() {
    let doc = ConfigDocument(text: "file.source = not-a-source\n")
    let config = DaemonConfig.from(doc)
    XCTAssertEqual(config.file.source, DaemonConfig.Route.default.source)
  }

  func testMalformedFormatFallsBackToDefault() {
    let doc = ConfigDocument(text: "file.format = not-a-format\n")
    let config = DaemonConfig.from(doc)
    XCTAssertEqual(config.file.format, DaemonConfig.Route.default.format)
  }

  func testMalformedTiffCompressionFallsBackToDefault() {
    let doc = ConfigDocument(text: "file.tiff_compression = not-a-codec\n")
    let config = DaemonConfig.from(doc)
    XCTAssertEqual(config.file.tiffCompression, DaemonConfig.Route.default.tiffCompression)
  }

  func testMalformedSeparationFallsBackToDefault() {
    let doc = ConfigDocument(text: "file.separation = not-a-separation\n")
    let config = DaemonConfig.from(doc)
    XCTAssertEqual(config.file.separation, DaemonConfig.Route.default.separation)
  }

  // MARK: Paper is stored raw, unvalidated (mirrors daemon/config.cpp)

  /// Unlike every other `<dest>.*` field, `<dest>.paper` is never validated
  /// by the daemon's `ApplyKey` -- an out-of-vocabulary token is still
  /// stored verbatim, not defaulted. `DaemonConfig` mirrors that exactly.
  func testUnrecognizedPaperTokenIsStoredVerbatimNotDefaulted() {
    let doc = ConfigDocument(text: "file.paper = NOT-A-KNOWN-PAPER\n")
    let config = DaemonConfig.from(doc)
    XCTAssertEqual(config.file.paper, "NOT-A-KNOWN-PAPER")
  }

  /// A missing `<dest>.paper` key, in contrast, does take the default
  /// (`""`, "no explicit paper").
  func testMissingPaperKeyTakesDefault() {
    let doc = ConfigDocument(text: "file.mode = color\n")
    let config = DaemonConfig.from(doc)
    XCTAssertEqual(config.file.paper, "")
  }

  // MARK: Boolean toggles: high_speed / skip_blank (Tasks 1e.16/1e.18)

  /// `<dest>.high_speed = on` parses to `highSpeed == true`; a missing key
  /// takes the default (`false`). Covered on both a plain route (file) and
  /// the OCR route.
  func testHighSpeedOnParsesTrueAndMissingDefaultsFalse() {
    let doc = ConfigDocument(text: "file.high_speed = on\nocr.high_speed = on\n")
    let config = DaemonConfig.from(doc)
    XCTAssertTrue(config.file.highSpeed)
    XCTAssertTrue(config.ocr.highSpeed)
    // image/email keys absent -> default false.
    XCTAssertFalse(config.image.highSpeed)
    XCTAssertFalse(config.email.highSpeed)
  }

  /// `<dest>.skip_blank = on` parses to `skipBlank == true`; a missing key
  /// takes the default (`false`). Covered on both a plain route (file) and
  /// the OCR route.
  func testSkipBlankOnParsesTrueAndMissingDefaultsFalse() {
    let doc = ConfigDocument(text: "file.skip_blank = on\nocr.skip_blank = on\n")
    let config = DaemonConfig.from(doc)
    XCTAssertTrue(config.file.skipBlank)
    XCTAssertTrue(config.ocr.skipBlank)
    XCTAssertFalse(config.image.skipBlank)
    XCTAssertFalse(config.email.skipBlank)
  }

  /// Setting `highSpeed`/`skipBlank` true on a route writes the canonical
  /// `on` value the daemon emits; false writes `off`. Checked on the file
  /// and OCR routes.
  func testApplyWritesOnOffForToggles() {
    var doc = ConfigDocument()
    var config = DaemonConfig.default
    config.file.highSpeed = true
    config.file.skipBlank = false
    config.ocr.highSpeed = false
    config.ocr.skipBlank = true
    config.apply(to: &doc)

    XCTAssertEqual(doc.value(for: "file.high_speed"), "on")
    XCTAssertEqual(doc.value(for: "file.skip_blank"), "off")
    XCTAssertEqual(doc.value(for: "ocr.high_speed"), "off")
    XCTAssertEqual(doc.value(for: "ocr.skip_blank"), "on")
  }

  /// An unparsable boolean value, like a missing key, keeps the default
  /// (`false`) -- mirroring the daemon's `ParseBoolString` leaving the field
  /// unchanged on failure.
  func testMalformedToggleFallsBackToDefault() {
    let doc = ConfigDocument(text: "file.high_speed = maybe\nfile.skip_blank = sometimes\n")
    let config = DaemonConfig.from(doc)
    XCTAssertFalse(config.file.highSpeed)
    XCTAssertFalse(config.file.skipBlank)
  }

  // MARK: Defaults sanity

  func testDefaultRouteMatchesDaemonDefaults() {
    let route = DaemonConfig.Route.default
    XCTAssertEqual(route.mode, "color")
    XCTAssertEqual(route.source, "flatbed")
    XCTAssertEqual(route.dpi, 300)
    XCTAssertEqual(route.format, "native")
    XCTAssertEqual(route.tiffCompression, "lzw")
    XCTAssertEqual(route.separation, .combine)
    XCTAssertEqual(route.paper, "")
    XCTAssertFalse(route.highSpeed)
    XCTAssertFalse(route.skipBlank)
  }

  func testDefaultGeneralHasNoPrinterHostOrDisplayName() {
    let general = DaemonConfig.default.general
    XCTAssertEqual(general.printerHost, "")
    XCTAssertEqual(general.displayName, "")
    XCTAssertEqual(general.saveDir, "~/Scans")
    XCTAssertEqual(general.imageApp, "")
    XCTAssertEqual(general.emailTo, "")
  }

  /// An entirely empty document parses to exactly `DaemonConfig.default`.
  func testEmptyDocumentParsesToDefault() {
    let doc = ConfigDocument(text: "")
    XCTAssertEqual(DaemonConfig.from(doc), .default)
  }
}
