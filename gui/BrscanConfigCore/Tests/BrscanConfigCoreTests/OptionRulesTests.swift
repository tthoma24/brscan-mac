import XCTest

@testable import BrscanConfigCore

/// Tests for `OptionRules` (Task 1e.3): which keys apply to which
/// `<dest>.format` value. Asserted for every generated format token, not
/// just tiff/pdf, so a new format added to `GeneratedOptionSets` later
/// gets an explicit answer here instead of falling through silently.
final class OptionRulesTests: XCTestCase {

  func testCompressionAppliesOnlyToTiff() {
    for format in OptionSets.format {
      let expected = (format == "tiff")
      XCTAssertEqual(
        OptionRules.compressionApplies(to: format), expected,
        "compressionApplies(to: \(format)) should be \(expected)")
    }
  }

  func testSeparationAppliesOnlyToPdfAndTiff() {
    for format in OptionSets.format {
      let expected = (format == "pdf" || format == "tiff")
      XCTAssertEqual(
        OptionRules.separationApplies(to: format), expected,
        "separationApplies(to: \(format)) should be \(expected)")
    }
  }

  func testSearchableAppliesOnlyToPdf() {
    for format in OptionSets.format {
      let expected = (format == "pdf")
      XCTAssertEqual(
        OptionRules.searchableApplies(to: format), expected,
        "searchableApplies(to: \(format)) should be \(expected)")
    }
  }
}
