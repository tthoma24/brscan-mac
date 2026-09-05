import XCTest

@testable import BrscanConfigCore

final class ConfigDocumentTests: XCTestCase {

  // MARK: Fixture loading

  private func fixtureURL() throws -> URL {
    try XCTUnwrap(
      Bundle.module.url(forResource: "sample-config", withExtension: "conf", subdirectory: "Fixtures"),
      "Missing Fixtures/sample-config.conf")
  }

  private func fixtureText() throws -> String {
    try String(contentsOf: fixtureURL(), encoding: .utf8)
  }

  // MARK: Round-trip identity

  /// Round-tripping an unmodified file returns identical bytes: every
  /// comment, blank line, key order, and unknown/commented-out key is
  /// preserved exactly.
  func testRoundTripIdentity() throws {
    let original = try fixtureText()
    let doc = ConfigDocument(text: original)

    XCTAssertEqual(doc.serialized(), original)

    // Byte-identical, not just String-equal: write it out and compare raw
    // bytes against the fixture on disk.
    let originalData = try Data(contentsOf: fixtureURL())
    let tempURL = FileManager.default.temporaryDirectory
      .appendingPathComponent("brscan-config-roundtrip-\(UUID().uuidString).conf")
    defer { try? FileManager.default.removeItem(at: tempURL) }
    try doc.write(to: tempURL)
    let writtenData = try Data(contentsOf: tempURL)
    XCTAssertEqual(writtenData, originalData)
  }

  // MARK: Setting an existing key

  /// Setting a value for a key that already has an active line changes
  /// only that line -- comments, order, and every other key stay
  /// byte-identical.
  func testSetExistingKeyChangesOnlyThatLine() throws {
    let original = try fixtureText()
    var doc = ConfigDocument(text: original)

    XCTAssertEqual(doc.value(for: "file.mode"), "color")
    doc.setValue("gray", for: "file.mode")
    XCTAssertEqual(doc.value(for: "file.mode"), "gray")

    let originalLines = original.components(separatedBy: "\n")
    let updatedLines = doc.serialized().components(separatedBy: "\n")
    XCTAssertEqual(updatedLines.count, originalLines.count, "line count must not change")

    var changedLineIndexes: [Int] = []
    for index in 0..<originalLines.count where originalLines[index] != updatedLines[index] {
      changedLineIndexes.append(index)
    }
    XCTAssertEqual(changedLineIndexes.count, 1, "exactly one line should differ")
    if let index = changedLineIndexes.first {
      XCTAssertEqual(originalLines[index], "file.mode = color")
      XCTAssertEqual(updatedLines[index], "file.mode = gray")
    }
  }

  /// Setting a value for a key with more than one active line (an earlier
  /// line shadowed by a later one, matching the daemon's last-one-wins
  /// parse) updates the line that actually takes effect.
  func testSetExistingKeyWithDuplicateLinesUpdatesTheWinningLine() {
    let text = [
      "file.mode = color",
      "file.mode = gray",
    ].joined(separator: "\n") + "\n"
    var doc = ConfigDocument(text: text)

    XCTAssertEqual(doc.value(for: "file.mode"), "gray")
    doc.setValue("bw", for: "file.mode")

    let expected = [
      "file.mode = color",
      "file.mode = bw",
    ].joined(separator: "\n") + "\n"
    XCTAssertEqual(doc.serialized(), expected)
  }

  // MARK: Setting an absent key

  /// Setting a value for a key with no active line at all appends a new
  /// line; existing content is untouched.
  func testSetAbsentKeyAppends() throws {
    let original = try fixtureText()
    var doc = ConfigDocument(text: original)

    XCTAssertNil(doc.value(for: "email.mode"))
    doc.setValue("bw", for: "email.mode")

    XCTAssertEqual(doc.serialized(), original + "email.mode = bw\n")
    XCTAssertEqual(doc.value(for: "email.mode"), "bw")
  }

  /// A commented-out example line for a key is never uncommented when
  /// that key is set -- a new active line is appended instead, so the
  /// example stays intact.
  func testSetAbsentKeyDoesNotUncommentExistingCommentedExample() throws {
    let original = try fixtureText()
    XCTAssertTrue(original.contains("# image_app = Preview"))
    var doc = ConfigDocument(text: original)

    XCTAssertNil(doc.value(for: "image_app"))
    doc.setValue("Preview", for: "image_app")

    let serialized = doc.serialized()
    XCTAssertTrue(serialized.contains("# image_app = Preview"), "the commented example must survive untouched")
    XCTAssertEqual(serialized, original + "image_app = Preview\n")
    XCTAssertEqual(doc.value(for: "image_app"), "Preview")
  }

  // MARK: Embedded line breaks are collapsed, not written raw (Review M1)

  /// A value containing an embedded `"\n"` (e.g. pasted multi-line text
  /// into a bound `TextField`) must not split the `key = value` line in
  /// two on disk -- that would corrupt the file, since the continuation
  /// would read back as an unrelated `.other` line. The line break is
  /// collapsed to a space instead.
  func testSetValueCollapsesEmbeddedNewlineInsteadOfSplittingTheLine() {
    var doc = ConfigDocument()
    doc.setValue("line one\nline two", for: "email_to")

    XCTAssertEqual(doc.serialized(), "email_to = line one line two")
    XCTAssertEqual(doc.value(for: "email_to"), "line one line two")
  }

  /// `"\r\n"` and a bare `"\r"` are collapsed the same way as `"\n"`.
  func testSetValueCollapsesCarriageReturnsToo() {
    var doc = ConfigDocument()
    doc.setValue("a\r\nb\rc", for: "email_to")

    XCTAssertEqual(doc.value(for: "email_to"), "a b c")
    XCTAssertFalse(doc.serialized().contains("\r"))
  }

  // MARK: Removing a key

  /// Removing a key with an active line deletes that line entirely --
  /// leaving the key absent, not present-with-an-empty-value -- while every
  /// other line stays byte-for-byte unchanged.
  func testRemoveValueDeletesTheActiveLine() throws {
    let original = try fixtureText()
    var doc = ConfigDocument(text: original)

    XCTAssertEqual(doc.value(for: "display_name"), "Test Mac")
    doc.removeValue(for: "display_name")

    XCTAssertNil(doc.value(for: "display_name"))
    XCTAssertFalse(doc.serialized().contains("display_name"), "the key must be gone, not just blanked")

    let originalLines = original.components(separatedBy: "\n")
    let updatedLines = doc.serialized().components(separatedBy: "\n")
    XCTAssertEqual(updatedLines.count, originalLines.count - 1, "exactly one line should be removed")
  }

  /// Removing a key that has no active line at all is a no-op -- the
  /// document is left byte-for-byte identical.
  func testRemoveValueOfAbsentKeyIsNoOp() throws {
    let original = try fixtureText()
    var doc = ConfigDocument(text: original)

    XCTAssertNil(doc.value(for: "does_not_exist"))
    doc.removeValue(for: "does_not_exist")

    XCTAssertEqual(doc.serialized(), original)
  }

  /// Removing a key with more than one active line (an earlier line
  /// shadowed by a later one) removes every line for that key, not just the
  /// winning one -- so the key is fully absent, with no shadowed duplicate
  /// left behind to resurface if a later `setValue` were ever applied.
  func testRemoveValueWithDuplicateLinesRemovesAllOfThem() {
    let text = [
      "file.mode = color",
      "file.mode = gray",
    ].joined(separator: "\n") + "\n"
    var doc = ConfigDocument(text: text)

    doc.removeValue(for: "file.mode")

    XCTAssertNil(doc.value(for: "file.mode"))
    // Every line is gone, but the file's trailing-newline style survives
    // independently of line content (see `hasTrailingNewline`).
    XCTAssertEqual(doc.serialized(), "\n")
  }

  // MARK: Whitespace and comment handling, mirroring daemon/config.cpp

  /// Values are trimmed the same way `daemon/config.cpp`'s `Trim()` trims
  /// them, and a line whose first non-blank character is `#` is a comment
  /// -- never an active key -- exactly matching `ParseConfig()`.
  func testWhitespaceAndCommentHandlingMirrorsDaemonParser() {
    let lines = [
      "# a leading comment",
      "   # an indented comment is still a comment",
      "printer_host=BRW00AABBCCDDEE.local",
      "   file.mode   =   color   ",
      "file.dpi=300",
      "#file.dpi=999",
      "   ",
      "",
      "email_to = ",
    ]
    let text = lines.joined(separator: "\n") + "\n"
    let doc = ConfigDocument(text: text)

    // Round-trips identically even with irregular internal whitespace,
    // since nothing here is being set.
    XCTAssertEqual(doc.serialized(), text)

    XCTAssertEqual(doc.value(for: "printer_host"), "BRW00AABBCCDDEE.local")
    // Whitespace around both the key and the value is trimmed.
    XCTAssertEqual(doc.value(for: "file.mode"), "color")
    // The commented-out override is ignored; the earlier active value
    // for the same key stands.
    XCTAssertEqual(doc.value(for: "file.dpi"), "300")
    // An empty value trims down to the empty string, not nil.
    XCTAssertEqual(doc.value(for: "email_to"), "")
    // A key that never appears (active or not) reads as nil.
    XCTAssertNil(doc.value(for: "does_not_exist"))
  }

  /// A `#` that isn't the first non-blank character of the line has no
  /// special meaning -- `daemon/config.cpp` only checks the trimmed
  /// line's first character, there is no inline-comment support.
  func testHashMidValueIsNotATrailingComment() {
    let text = "email_to = a#b\n"
    let doc = ConfigDocument(text: text)
    XCTAssertEqual(doc.value(for: "email_to"), "a#b")
  }

  // MARK: Disk round-trip

  func testWriteThenLoadRoundTrip() throws {
    var doc = ConfigDocument(text: try fixtureText())
    doc.setValue("gray", for: "file.mode")
    doc.setValue("bw", for: "email.mode")

    let tempURL = FileManager.default.temporaryDirectory
      .appendingPathComponent("brscan-config-write-load-\(UUID().uuidString).conf")
    defer { try? FileManager.default.removeItem(at: tempURL) }

    try doc.write(to: tempURL)
    let reloaded = try ConfigDocument.load(from: tempURL)

    XCTAssertEqual(reloaded.serialized(), doc.serialized())
    XCTAssertEqual(reloaded.value(for: "file.mode"), "gray")
    XCTAssertEqual(reloaded.value(for: "email.mode"), "bw")
  }

  // MARK: Empty document

  func testEmptyDocumentSerializesToEmptyString() {
    let doc = ConfigDocument()
    XCTAssertEqual(doc.serialized(), "")
  }

  func testEmptyTextRoundTrips() {
    let doc = ConfigDocument(text: "")
    XCTAssertEqual(doc.serialized(), "")
  }
}
