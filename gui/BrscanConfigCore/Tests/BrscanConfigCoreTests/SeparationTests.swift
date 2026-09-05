import XCTest

@testable import BrscanConfigCore

/// Tests for `Separation`/`SeparationCodec` (Task 1e.3), mirroring
/// daemon/config.cpp's `ParseSeparationString()`.
final class SeparationTests: XCTestCase {

  // MARK: parse

  func testCombineAndOffBothParseToCombine() {
    XCTAssertEqual(SeparationCodec.parse("combine"), .combine)
    XCTAssertEqual(SeparationCodec.parse("off"), .combine)
  }

  func testImageWithCountParsesToImage() {
    XCTAssertEqual(SeparationCodec.parse("image:2"), .image(2))
  }

  func testPageWithCountParsesToPage() {
    XCTAssertEqual(SeparationCodec.parse("page:3"), .page(3))
  }

  func testEveryIsABackwardCompatAliasForImage() {
    XCTAssertEqual(SeparationCodec.parse("every:4"), .image(4))
  }

  func testRejectsZeroCount() {
    XCTAssertNil(SeparationCodec.parse("image:0"))
  }

  func testRejectsNonNumericCount() {
    XCTAssertNil(SeparationCodec.parse("image:x"))
  }

  func testRejectsMissingCount() {
    XCTAssertNil(SeparationCodec.parse("image"))
    XCTAssertNil(SeparationCodec.parse("image:"))
  }

  func testRejectsUnrecognizedMode() {
    XCTAssertNil(SeparationCodec.parse("ZZZ"))
  }

  // MARK: serialize / round-trip

  func testSerializeCombine() {
    XCTAssertEqual(SeparationCodec.serialize(.combine), "combine")
  }

  func testSerializeImageAndPage() {
    XCTAssertEqual(SeparationCodec.serialize(.image(4)), "image:4")
    XCTAssertEqual(SeparationCodec.serialize(.page(7)), "page:7")
  }

  func testRoundTripsImagePageAndCombine() {
    for value: Separation in [.combine, .image(4), .page(7)] {
      let serialized = SeparationCodec.serialize(value)
      XCTAssertEqual(SeparationCodec.parse(serialized), value)
    }
  }

  func testEveryNeverReemittedOnSerialize() {
    // "every:4" parses in as .image(4), but serializing that value back
    // out always spells it "image:4" -- "every" is parse-only.
    let parsed = SeparationCodec.parse("every:4")
    XCTAssertEqual(parsed, .image(4))
    XCTAssertEqual(SeparationCodec.serialize(parsed!), "image:4")
  }
}
