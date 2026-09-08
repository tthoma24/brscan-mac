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

  func testJpegQualityAppliesToLossyImageFormats() {
    let lossy: Set<String> = ["jpeg", "heic", "jp2"]
    for format in OptionSets.format {
      let expected = lossy.contains(format)
      XCTAssertEqual(
        OptionRules.jpegQualityApplies(to: format), expected,
        "jpegQualityApplies(to: \(format)) should be \(expected)")
    }
  }

  // MARK: allowedFormats(forMode:)

  /// Formats every mode can produce (all pixel classes): the lossless and
  /// container formats, plus `native`.
  private static let universalFormats = ["native", "pdf", "tiff", "png", "gif", "bmp"]

  func testColorModeAllowsEveryFormat() {
    XCTAssertEqual(OptionRules.allowedFormats(forMode: "color"), OptionSets.format)
  }

  func testGrayscaleModesAllowEveryFormatExceptHeic() {
    let expected = OptionSets.format.filter { $0 != "heic" }
    for mode in ["gray", "truegray"] {
      XCTAssertEqual(
        OptionRules.allowedFormats(forMode: mode), expected,
        "allowedFormats(forMode: \(mode)) should exclude only heic")
    }
  }

  func testBitonalModesExcludeLossyPhotoFormats() {
    let expected = OptionSets.format.filter { !["heic", "jpeg", "jp2"].contains($0) }
    for mode in ["bw", "errdiff"] {
      XCTAssertEqual(
        OptionRules.allowedFormats(forMode: mode), expected,
        "allowedFormats(forMode: \(mode)) should exclude heic/jpeg/jp2")
      XCTAssertEqual(
        OptionRules.allowedFormats(forMode: mode), Self.universalFormats,
        "the bitonal set is exactly the universal formats")
    }
  }

  /// An unrecognized mode falls back to the most permissive (color) set so
  /// nothing is wrongly hidden.
  func testUnknownModeDefaultsToEveryFormat() {
    XCTAssertEqual(OptionRules.allowedFormats(forMode: "sepia"), OptionSets.format)
  }

  /// `pdf`, the view model's coercion fallback, is valid in every mode.
  func testPdfIsAllowedForEveryMode() {
    for mode in OptionSets.mode {
      XCTAssertTrue(
        OptionRules.allowedFormats(forMode: mode).contains("pdf"),
        "pdf should be allowed for mode \(mode)")
    }
  }
}
