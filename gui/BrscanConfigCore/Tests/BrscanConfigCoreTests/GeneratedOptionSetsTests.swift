import XCTest

@testable import BrscanConfigCore

/// Smoke tests for the generated `OptionSets` constants (Task 1e.1). The
/// real anti-drift coverage lives in two other places: CMakeLists.txt's
/// `OptionSetsSwiftConstantsUpToDate` CTest entry (regenerates this file
/// from config/option-sets.json and diffs against the committed copy) and
/// tests/option_sets_test.cpp (asserts the daemon's own parsers actually
/// accept every token). This file just guards against `OptionSets` being
/// empty or obviously wrong from the Swift side.
final class GeneratedOptionSetsTests: XCTestCase {

  func testTokenSetsAreNonEmpty() {
    XCTAssertFalse(OptionSets.mode.isEmpty)
    XCTAssertFalse(OptionSets.source.isEmpty)
    XCTAssertFalse(OptionSets.format.isEmpty)
    XCTAssertFalse(OptionSets.tiffCompression.isEmpty)
    XCTAssertFalse(OptionSets.separationMode.isEmpty)
    XCTAssertFalse(OptionSets.paper.isEmpty)
  }

  func testPaperHasTheNineDocumentedTokens() {
    // daemon/paper_size.h's kPaperTable has exactly 9 entries -- see that
    // file's IsKnownPaper doc comment.
    XCTAssertEqual(Set(OptionSets.paper).count, 9)
  }

  func testSeparationModesAreTheThreeVendorModesNotAliases() {
    // "every" is a backward-compat alias for "image", not a fourth mode --
    // see config/option-sets.json's separation_mode._comment.
    XCTAssertEqual(Set(OptionSets.separationMode), ["combine", "off", "image", "page"])
  }

  func testDpiDefaultMatchesDaemonDefault() {
    // brscan::Params's own default constructor is 300dpi (see
    // daemon/config.h's Config::file_params doc comment).
    XCTAssertEqual(OptionSets.dpiDefault, 300)
  }
}
