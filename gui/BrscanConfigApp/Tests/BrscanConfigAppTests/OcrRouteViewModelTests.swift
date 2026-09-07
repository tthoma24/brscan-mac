import XCTest

@testable import BrscanConfigApp
import BrscanConfigCore

/// Unit tests for `OcrRouteViewModel`: the OCR sub-format picker
/// (`ocr.ocr_format`: pdf/txt/html/rtf) round-tripping, the image `format`
/// field pinned to `pdf` (the daemon ignores `ocr.format` for OCR),
/// separation gating by sub-format, mode/dpi/source/paper editing and
/// round-trip, and the OCR note's static content.
final class OcrRouteViewModelTests: XCTestCase {

  // MARK: OCR sub-format is a real, round-tripping choice (Task 1e.15)

  func testDefaultSubFormatIsPdf() {
    let viewModel = OcrRouteViewModel()
    XCTAssertEqual(viewModel.format, "pdf")
  }

  /// Picking `rtf` produces a route whose serialized OCR line is
  /// `ocr.ocr_format = rtf` -- the daemon's Task 1e.14 key.
  func testSelectingRtfProducesTheOcrFormatConfigLine() {
    let viewModel = OcrRouteViewModel()
    viewModel.format = "rtf"

    XCTAssertEqual(viewModel.route.ocrFormat, "rtf")

    var config = DaemonConfig.default
    config.ocr = viewModel.route
    var doc = ConfigDocument()
    config.apply(to: &doc)

    XCTAssertEqual(doc.value(for: "ocr.ocr_format"), "rtf")
    XCTAssertTrue(doc.serialized().contains("ocr.ocr_format = rtf"))
  }

  /// Loading a config document with `ocr.ocr_format = html` selects `html`
  /// in the view model.
  func testLoadingHtmlOcrFormatSelectsHtml() {
    let doc = ConfigDocument(text: "ocr.ocr_format = html\n")
    let config = DaemonConfig.from(doc)

    let viewModel = OcrRouteViewModel(route: config.ocr)
    XCTAssertEqual(viewModel.format, "html")

    // The in-place reseed path (ConfigStore reloads) picks it up too.
    let reseeded = OcrRouteViewModel()
    reseeded.load(config.ocr)
    XCTAssertEqual(reseeded.format, "html")
  }

  /// Every generated OCR sub-format token round-trips through the produced
  /// route's `ocrFormat`, and the image `format` field stays pinned to
  /// `pdf` (the daemon ignores `ocr.format` for OCR -- see docs/BUTTON.md).
  func testEveryOcrFormatRoundTripsAndImageFormatStaysPdf() {
    for token in OptionSets.ocrFormat {
      let viewModel = OcrRouteViewModel()
      viewModel.format = token
      XCTAssertEqual(viewModel.route.ocrFormat, token)
      XCTAssertEqual(viewModel.route.format, "pdf", "image format leaked for sub-format \(token)")
    }
  }

  // MARK: high_speed / skip_blank toggles (Tasks 1e.16/1e.18)

  /// Setting the OCR route's toggles true produces the canonical
  /// `ocr.high_speed = on` / `ocr.skip_blank = on` config lines.
  func testSettingTogglesProducesTheOcrConfigLines() {
    let viewModel = OcrRouteViewModel()
    XCTAssertFalse(viewModel.highSpeed)
    XCTAssertFalse(viewModel.skipBlank)

    viewModel.highSpeed = true
    viewModel.skipBlank = true

    var config = DaemonConfig.default
    config.ocr = viewModel.route
    var doc = ConfigDocument()
    config.apply(to: &doc)

    XCTAssertEqual(doc.value(for: "ocr.high_speed"), "on")
    XCTAssertEqual(doc.value(for: "ocr.skip_blank"), "on")
  }

  /// Loading a document with `ocr.skip_blank = on` selects `skipBlank` in
  /// the view model, via both the seeding init and the in-place reseed.
  func testLoadingSkipBlankOnSelectsIt() {
    let doc = ConfigDocument(text: "ocr.skip_blank = on\nocr.high_speed = on\n")
    let config = DaemonConfig.from(doc)

    let viewModel = OcrRouteViewModel(route: config.ocr)
    XCTAssertTrue(viewModel.skipBlank)
    XCTAssertTrue(viewModel.highSpeed)

    let reseeded = OcrRouteViewModel()
    reseeded.load(config.ocr)
    XCTAssertTrue(reseeded.skipBlank)
    XCTAssertTrue(reseeded.highSpeed)
  }

  // MARK: Separation gating by sub-format

  func testSeparationIsEditableSincePdfIsAContainerFormat() {
    let viewModel = OcrRouteViewModel()
    XCTAssertTrue(viewModel.isSeparationEditable)
    XCTAssertTrue(OptionRules.separationApplies(to: viewModel.format))
  }

  func testSeparationIsNotEditableForTextSinkSubFormats() {
    let viewModel = OcrRouteViewModel()
    for token in ["txt", "html", "rtf"] {
      viewModel.format = token
      XCTAssertFalse(viewModel.isSeparationEditable, "\(token) is not a container format")
    }
  }

  func testSearchablePdfNoteAppliesOnlyToPdf() {
    let viewModel = OcrRouteViewModel()
    XCTAssertTrue(viewModel.isSearchablePdf)
    for token in ["txt", "html", "rtf"] {
      viewModel.format = token
      XCTAssertFalse(viewModel.isSearchablePdf, "\(token) has no searchable-PDF text layer")
    }
  }

  func testSwitchingSeparationModesProducesTheExpectedSeparation() {
    let viewModel = OcrRouteViewModel()

    viewModel.separationMode = .image
    viewModel.separationCount = 4
    XCTAssertEqual(viewModel.separation, .image(4))
    XCTAssertEqual(viewModel.route.separation, .image(4))

    viewModel.separationMode = .page
    viewModel.separationCount = 7
    XCTAssertEqual(viewModel.separation, .page(7))
    XCTAssertEqual(viewModel.route.separation, .page(7))

    viewModel.separationMode = .combine
    XCTAssertEqual(viewModel.separation, .combine)
    XCTAssertEqual(viewModel.route.separation, .combine)
  }

  func testSeedingFromPageSeparationPopulatesModeAndCount() {
    let seed = DaemonConfig.Route(
      mode: "bw", source: "adf", dpi: 200, format: "native", tiffCompression: "g3", separation: .page(3),
      paper: "A4")

    let viewModel = OcrRouteViewModel(route: seed)

    XCTAssertEqual(viewModel.separationMode, .page)
    XCTAssertEqual(viewModel.separationCount, 3)
  }

  // MARK: Validation

  func testZeroOrNegativeSeparationCountIsInvalid() {
    let viewModel = OcrRouteViewModel()

    viewModel.separationCount = 0
    XCTAssertFalse(viewModel.isSeparationCountValid)

    viewModel.separationCount = 5
    XCTAssertTrue(viewModel.isSeparationCountValid)
  }

  // MARK: Scan params edit and round-trip as usual

  func testSettingEachScanParamUpdatesTheProducedRoute() {
    let viewModel = OcrRouteViewModel()

    viewModel.mode = "gray"
    viewModel.source = "adf-duplex"
    viewModel.dpi = 600
    viewModel.paper = "LETTER"

    let route = viewModel.route
    XCTAssertEqual(route.mode, "gray")
    XCTAssertEqual(route.source, "adf-duplex")
    XCTAssertEqual(route.dpi, 600)
    XCTAssertEqual(route.paper, "LETTER")
    XCTAssertEqual(route.format, "pdf")
  }

  func testSeedingFromRoutePopulatesScanParamFields() {
    let seed = DaemonConfig.Route(
      mode: "errdiff", source: "flatbed", dpi: 400, format: "jpeg", tiffCompression: "lzw",
      separation: .image(5), paper: "LEGAL")

    let viewModel = OcrRouteViewModel(route: seed)

    XCTAssertEqual(viewModel.mode, seed.mode)
    XCTAssertEqual(viewModel.source, seed.source)
    XCTAssertEqual(viewModel.dpi, seed.dpi)
    XCTAssertEqual(viewModel.paper, seed.paper)
  }

  func testSeedingThenProducingRouteRoundTripsEveryFieldExceptFormat() {
    let seed = DaemonConfig.Route(
      mode: "errdiff", source: "flatbed", dpi: 400, format: "pdf", tiffCompression: "lzw",
      separation: .image(5), paper: "LEGAL")

    let viewModel = OcrRouteViewModel(route: seed)

    // Every field round-trips except format, which the seed already had at
    // "pdf" here -- so the whole route matches, including format.
    XCTAssertEqual(viewModel.route, seed)
  }

  func testPositiveDpiIsValid() {
    let viewModel = OcrRouteViewModel()
    viewModel.dpi = 300
    XCTAssertTrue(viewModel.isDpiValid)

    viewModel.dpi = 0
    XCTAssertFalse(viewModel.isDpiValid)
  }

  // MARK: Note static content

  /// A small static-content assertion (per the brief) that the exact note
  /// `OcrTabView` renders (`OcrRouteViewModel.Notes`, the single source of
  /// truth both this test and the view read) carries the expected claim --
  /// not a UI-rendering test (this package doesn't launch any UI), just
  /// pinning the key substance so it can't silently drift.
  func testOcrTabSearchablePdfNoteDescribesTheTextLayer() {
    XCTAssertTrue(OcrRouteViewModel.Notes.searchablePdf.contains("Searchable PDF"))
    XCTAssertTrue(OcrRouteViewModel.Notes.searchablePdf.contains("text layer"))
  }
}
