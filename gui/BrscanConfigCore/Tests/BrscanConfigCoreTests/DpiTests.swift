import XCTest

@testable import BrscanConfigCore

/// Tests for `Dpi` (Task 1e.3): a positive-int validator/parser, not an
/// enumerated set -- daemon/config.cpp's `ParsePositiveInt()` accepts any
/// positive integer for `<dest>.dpi`.
final class DpiTests: XCTestCase {

  func testPositiveValuesAreValid() {
    XCTAssertTrue(Dpi.isValid(1))
    XCTAssertTrue(Dpi.isValid(300))
    XCTAssertTrue(Dpi.isValid(OptionSets.dpiDefault))
  }

  func testZeroAndNegativeValuesAreInvalid() {
    XCTAssertFalse(Dpi.isValid(0))
    XCTAssertFalse(Dpi.isValid(-1))
    XCTAssertFalse(Dpi.isValid(-300))
  }

  func testParseAcceptsPositiveIntegerStrings() {
    XCTAssertEqual(Dpi.parse("300"), 300)
    XCTAssertEqual(Dpi.parse("1"), 1)
  }

  func testParseRejectsZeroNegativeAndNonNumericStrings() {
    XCTAssertNil(Dpi.parse("0"))
    XCTAssertNil(Dpi.parse("-1"))
    XCTAssertNil(Dpi.parse("x"))
    XCTAssertNil(Dpi.parse(""))
    XCTAssertNil(Dpi.parse("300dpi"))
  }

  /// The daemon's `ParsePositiveInt()` uses 32-bit `std::stoi`, which throws
  /// `out_of_range` for a value above `INT_MAX` (dropping the key), so `Dpi`
  /// must reject `> Int32.max` even though Swift's `Int` could hold it --
  /// otherwise the GUI would call a value "valid" that the daemon discards
  /// (Review finding G1).
  func testInt32MaxIsTheUpperBound() {
    XCTAssertTrue(Dpi.isValid(Int(Int32.max)))
    XCTAssertEqual(Dpi.parse(String(Int32.max)), Int(Int32.max))

    XCTAssertFalse(Dpi.isValid(Int(Int32.max) + 1))
    XCTAssertNil(Dpi.parse(String(Int(Int32.max) + 1)))
    XCTAssertNil(Dpi.parse("9999999999"))
  }
}
