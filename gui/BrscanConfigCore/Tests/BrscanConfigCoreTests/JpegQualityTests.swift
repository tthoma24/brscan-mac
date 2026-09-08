import XCTest

@testable import BrscanConfigCore

/// Tests for `JpegQuality`: a bounded-int validator/parser for
/// `<dest>.jpeg_quality`, clamped to `0...100` the way daemon/config.cpp's
/// `ParseJpegQuality()` clamps it.
final class JpegQualityTests: XCTestCase {

  func testParseAcceptsAndClampsInRangeValues() {
    XCTAssertEqual(JpegQuality.parse("0"), 0)
    XCTAssertEqual(JpegQuality.parse("90"), 90)
    XCTAssertEqual(JpegQuality.parse("100"), 100)
    XCTAssertEqual(JpegQuality.parse("150"), 100)
    XCTAssertEqual(JpegQuality.parse("-5"), 0)
  }

  func testParseRejectsNonIntegerStrings() {
    XCTAssertNil(JpegQuality.parse(""))
    XCTAssertNil(JpegQuality.parse("high"))
    XCTAssertNil(JpegQuality.parse("90q"))
  }

  /// The daemon's `ParseJpegQuality()` uses 32-bit `std::stoi`, which throws
  /// `out_of_range` for a value outside `INT_MIN...INT_MAX` -- so it leaves
  /// the field at its default (90) rather than clamping. `JpegQuality.parse`
  /// must reject `> Int32.max` (and `< Int32.min`) before clamping too, so
  /// the GUI reads e.g. `3000000000` as the default rather than a clamped
  /// 100 (Review finding R6, #136). Mirrors `Dpi`'s 32-bit guard (#120).
  func testParseRejectsValuesOutsideInt32Range() {
    XCTAssertEqual(JpegQuality.parse(String(Int32.max)), 100)
    XCTAssertEqual(JpegQuality.parse(String(Int32.min)), 0)

    XCTAssertNil(JpegQuality.parse(String(Int(Int32.max) + 1)))
    XCTAssertNil(JpegQuality.parse(String(Int(Int32.min) - 1)))
    XCTAssertNil(JpegQuality.parse("3000000000"))
  }
}
