import XCTest

@testable import BrscanConfigCore

/// Tests for `OptionValueSets` (Task 1e.3). The core assertion these guard
/// is the anti-drift one: each value set's `options` must literally *be*
/// the corresponding `GeneratedOptionSets` array, not a hand-copied list
/// that could fall out of sync with it.
final class OptionValueSetsTests: XCTestCase {

  // MARK: options == GeneratedOptionSets (anti-drift)

  func testModeOptionsComeFromGeneratedOptionSets() {
    XCTAssertEqual(OptionValueSets.mode.options, OptionSets.mode)
  }

  func testSourceOptionsComeFromGeneratedOptionSets() {
    XCTAssertEqual(OptionValueSets.source.options, OptionSets.source)
  }

  func testFormatOptionsComeFromGeneratedOptionSets() {
    XCTAssertEqual(OptionValueSets.format.options, OptionSets.format)
  }

  func testTiffCompressionOptionsComeFromGeneratedOptionSets() {
    XCTAssertEqual(OptionValueSets.tiffCompression.options, OptionSets.tiffCompression)
  }

  func testPaperOptionsComeFromGeneratedOptionSets() {
    XCTAssertEqual(OptionValueSets.paper.options, OptionSets.paper)
  }

  func testOcrFormatOptionsComeFromGeneratedOptionSets() {
    XCTAssertEqual(OptionValueSets.ocrFormat.options, OptionSets.ocrFormat)
  }

  // MARK: isValid -- every generated token validates true

  func testEveryModeTokenIsValid() {
    for token in OptionSets.mode {
      XCTAssertTrue(OptionValueSets.mode.isValid(token), "\(token) should be a valid mode")
    }
  }

  func testEverySourceTokenIsValid() {
    for token in OptionSets.source {
      XCTAssertTrue(OptionValueSets.source.isValid(token), "\(token) should be a valid source")
    }
  }

  func testEveryFormatTokenIsValid() {
    for token in OptionSets.format {
      XCTAssertTrue(OptionValueSets.format.isValid(token), "\(token) should be a valid format")
    }
  }

  func testEveryTiffCompressionTokenIsValid() {
    for token in OptionSets.tiffCompression {
      XCTAssertTrue(OptionValueSets.tiffCompression.isValid(token), "\(token) should be a valid tiff_compression")
    }
  }

  func testEveryPaperTokenIsValid() {
    for token in OptionSets.paper {
      XCTAssertTrue(OptionValueSets.paper.isValid(token), "\(token) should be a valid paper")
    }
  }

  func testEveryOcrFormatTokenIsValid() {
    for token in OptionSets.ocrFormat {
      XCTAssertTrue(OptionValueSets.ocrFormat.isValid(token), "\(token) should be a valid ocr_format")
    }
  }

  // MARK: isValid -- a sentinel token is rejected everywhere

  func testSentinelTokenIsInvalidEverywhere() {
    XCTAssertFalse(OptionValueSets.mode.isValid("ZZZ"))
    XCTAssertFalse(OptionValueSets.source.isValid("ZZZ"))
    XCTAssertFalse(OptionValueSets.format.isValid("ZZZ"))
    XCTAssertFalse(OptionValueSets.tiffCompression.isValid("ZZZ"))
    XCTAssertFalse(OptionValueSets.paper.isValid("ZZZ"))
    XCTAssertFalse(OptionValueSets.ocrFormat.isValid("ZZZ"))
  }
}
