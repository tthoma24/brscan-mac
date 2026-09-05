import XCTest

@testable import BrscanConfigApp
import BrscanConfigCore

/// Unit tests for `OcrRouteViewModel`: format forced to `"pdf"` with no
/// format choice offered, separation still applying (container format),
/// mode/dpi/source/paper editing and round-trip, and the OCR notes' static
/// content -- per task 1e.8's brief.
final class OcrRouteViewModelTests: XCTestCase {

  // MARK: Format is forced, never a free choice

  /// There is no settable `format` property on this type at all (unlike
  /// `RouteViewModel`, which has a `@Published var format`) -- the type's
  /// shape itself is "no format choice offered".
  func testFormatIsAConstantNotAPublishedVar() {
    let viewModel = OcrRouteViewModel()
    XCTAssertEqual(viewModel.format, "pdf")
  }

  func testProducedRouteIsAlwaysPdfRegardlessOfSeedFormat() {
    // A hand-edited `ocr.format=tiff` (or any other non-pdf token) in the
    // config file -- the daemon ignores this override for OCR today (see
    // docs/BUTTON.md), so the view model must not perpetuate it on save.
    for seedFormat in OptionSets.format {
      let seed = DaemonConfig.Route(
        mode: "color", source: "flatbed", dpi: 300, format: seedFormat, tiffCompression: "lzw",
        separation: .combine, paper: "")
      let viewModel = OcrRouteViewModel(route: seed)

      XCTAssertEqual(viewModel.route.format, "pdf", "seed format \(seedFormat) leaked into the produced route")
    }
  }

  func testDefaultInitProducesPdfFormat() {
    let viewModel = OcrRouteViewModel()
    XCTAssertEqual(viewModel.route.format, "pdf")
  }

  // MARK: Separation still applies (pdf is a container format)

  func testSeparationIsEditableSincePdfIsAContainerFormat() {
    let viewModel = OcrRouteViewModel()
    XCTAssertTrue(viewModel.isSeparationEditable)
    XCTAssertTrue(OptionRules.separationApplies(to: viewModel.format))
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

  // MARK: Notes/caveats static content

  /// A small static-content assertion (per the brief) that the exact notes
  /// `OcrTabView` renders (`OcrRouteViewModel.Notes`, the single source of
  /// truth both this test and the view read) carry the expected claims --
  /// not a UI-rendering test (this package doesn't launch any UI), just
  /// pinning the key substance so it can't silently drift from what the
  /// daemon actually does.
  func testOcrTabNotesDescribeSearchablePdfAndUnproducedSubFormats() {
    XCTAssertTrue(OcrRouteViewModel.Notes.searchablePdf.contains("searchable PDF"))
    XCTAssertTrue(OcrRouteViewModel.Notes.searchablePdf.contains("text layer"))
    XCTAssertTrue(OcrRouteViewModel.Notes.subFormatsNotProduced.contains("Text, HTML, or RTF"))
    XCTAssertTrue(OcrRouteViewModel.Notes.subFormatsNotProduced.contains("isn't produced by this daemon yet"))
  }
}
