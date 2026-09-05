import XCTest

@testable import BrscanConfigApp

/// Proves the tab shell's structure without launching any UI: the five
/// `Tabs` cases exist, in the brief's order, with the expected titles.
final class TabsTests: XCTestCase {
  func testAllFiveTabsExistInOrder() {
    XCTAssertEqual(Tabs.allCases, [.general, .file, .image, .ocr, .email])
  }

  func testTabTitles() {
    XCTAssertEqual(Tabs.general.title, "General")
    XCTAssertEqual(Tabs.file.title, "File")
    XCTAssertEqual(Tabs.image.title, "Image")
    XCTAssertEqual(Tabs.ocr.title, "OCR")
    XCTAssertEqual(Tabs.email.title, "Email")
  }
}
