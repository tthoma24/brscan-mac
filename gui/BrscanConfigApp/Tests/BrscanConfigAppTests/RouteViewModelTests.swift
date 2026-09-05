import XCTest

@testable import BrscanConfigApp
import BrscanConfigCore

/// Unit tests for `RouteViewModel`: format gating (`OptionRules`), binding,
/// seed/round-trip from `DaemonConfig.Route`, separation editing, and dpi/
/// separation-count validation. No UI is launched -- these exercise the
/// view model directly, per task 1e.7's brief.
final class RouteViewModelTests: XCTestCase {

  // MARK: Gating

  func testTiffFormatMakesCompressionAndSeparationEditable() {
    let viewModel = RouteViewModel()
    viewModel.format = "tiff"

    XCTAssertTrue(viewModel.isCompressionEditable)
    XCTAssertTrue(viewModel.isSeparationEditable)
  }

  func testPdfFormatMakesSeparationEditableButNotCompression() {
    let viewModel = RouteViewModel()
    viewModel.format = "pdf"

    XCTAssertFalse(viewModel.isCompressionEditable)
    XCTAssertTrue(viewModel.isSeparationEditable)
  }

  func testJpegPngNativeFormatsMakeNeitherCompressionNorSeparationEditable() {
    for format in ["jpeg", "png", "native"] {
      let viewModel = RouteViewModel()
      viewModel.format = format

      XCTAssertFalse(viewModel.isCompressionEditable, "compression should not be editable for \(format)")
      XCTAssertFalse(viewModel.isSeparationEditable, "separation should not be editable for \(format)")
    }
  }

  func testGatingForEveryKnownFormat() {
    for format in OptionSets.format {
      let viewModel = RouteViewModel()
      viewModel.format = format

      XCTAssertEqual(
        viewModel.isCompressionEditable, OptionRules.compressionApplies(to: format),
        "compression gating mismatch for \(format)")
      XCTAssertEqual(
        viewModel.isSeparationEditable, OptionRules.separationApplies(to: format),
        "separation gating mismatch for \(format)")
    }
  }

  // MARK: Binding

  func testSettingEachFieldUpdatesTheProducedRoute() {
    let viewModel = RouteViewModel()

    viewModel.mode = "gray"
    viewModel.source = "adf-duplex"
    viewModel.dpi = 600
    viewModel.format = "tiff"
    viewModel.tiffCompression = "g4"
    viewModel.paper = "LETTER"

    let route = viewModel.route
    XCTAssertEqual(route.mode, "gray")
    XCTAssertEqual(route.source, "adf-duplex")
    XCTAssertEqual(route.dpi, 600)
    XCTAssertEqual(route.format, "tiff")
    XCTAssertEqual(route.tiffCompression, "g4")
    XCTAssertEqual(route.paper, "LETTER")
  }

  // MARK: Seeding / round-trip

  func testSeedingFromRoutePopulatesFields() {
    let seed = DaemonConfig.Route(
      mode: "bw", source: "adf", dpi: 200, format: "pdf", tiffCompression: "g3", separation: .page(3),
      paper: "A4")

    let viewModel = RouteViewModel(route: seed)

    XCTAssertEqual(viewModel.mode, seed.mode)
    XCTAssertEqual(viewModel.source, seed.source)
    XCTAssertEqual(viewModel.dpi, seed.dpi)
    XCTAssertEqual(viewModel.format, seed.format)
    XCTAssertEqual(viewModel.tiffCompression, seed.tiffCompression)
    XCTAssertEqual(viewModel.paper, seed.paper)
    XCTAssertEqual(viewModel.separationMode, .page)
    XCTAssertEqual(viewModel.separationCount, 3)
  }

  func testSeedingThenProducingRouteRoundTrips() {
    let seed = DaemonConfig.Route(
      mode: "errdiff", source: "flatbed", dpi: 400, format: "tiff", tiffCompression: "lzw",
      separation: .image(5), paper: "LEGAL")

    let viewModel = RouteViewModel(route: seed)

    XCTAssertEqual(viewModel.route, seed)
  }

  func testCombineSeparationRoundTrips() {
    let seed = DaemonConfig.Route(
      mode: "color", source: "flatbed", dpi: 300, format: "native", tiffCompression: "lzw",
      separation: .combine, paper: "")

    let viewModel = RouteViewModel(route: seed)

    XCTAssertEqual(viewModel.separationMode, .combine)
    XCTAssertEqual(viewModel.route, seed)
  }

  func testDefaultInitSeedsFromDaemonConfigRouteDefault() {
    let viewModel = RouteViewModel()
    XCTAssertEqual(viewModel.route, DaemonConfig.Route.default)
  }

  // MARK: Separation editing

  func testSwitchingToImageModeProducesImageSeparation() {
    let viewModel = RouteViewModel()
    viewModel.separationMode = .image
    viewModel.separationCount = 4

    XCTAssertEqual(viewModel.separation, .image(4))
    XCTAssertEqual(SeparationCodec.serialize(viewModel.separation), "image:4")
  }

  func testSwitchingToPageModeProducesPageSeparation() {
    let viewModel = RouteViewModel()
    viewModel.separationMode = .page
    viewModel.separationCount = 7

    XCTAssertEqual(viewModel.separation, .page(7))
    XCTAssertEqual(SeparationCodec.serialize(viewModel.separation), "page:7")
  }

  func testSwitchingBackToCombineProducesCombineSeparationRegardlessOfCount() {
    let viewModel = RouteViewModel()
    viewModel.separationMode = .image
    viewModel.separationCount = 9
    viewModel.separationMode = .combine

    XCTAssertEqual(viewModel.separation, .combine)
    XCTAssertEqual(SeparationCodec.serialize(viewModel.separation), "combine")
  }

  func testChangingCountUpdatesSeparationForCurrentMode() {
    let viewModel = RouteViewModel()
    viewModel.separationMode = .image
    viewModel.separationCount = 2
    XCTAssertEqual(viewModel.separation, .image(2))

    viewModel.separationCount = 10
    XCTAssertEqual(viewModel.separation, .image(10))
  }

  // MARK: Validation

  func testPositiveDpiIsValid() {
    let viewModel = RouteViewModel()
    viewModel.dpi = 300
    XCTAssertTrue(viewModel.isDpiValid)
  }

  func testZeroOrNegativeDpiIsInvalid() {
    let viewModel = RouteViewModel()

    viewModel.dpi = 0
    XCTAssertFalse(viewModel.isDpiValid)

    viewModel.dpi = -100
    XCTAssertFalse(viewModel.isDpiValid)
  }

  func testPositiveSeparationCountIsValid() {
    let viewModel = RouteViewModel()
    viewModel.separationCount = 5
    XCTAssertTrue(viewModel.isSeparationCountValid)
  }

  func testZeroOrNegativeSeparationCountIsInvalid() {
    let viewModel = RouteViewModel()

    viewModel.separationCount = 0
    XCTAssertFalse(viewModel.isSeparationCountValid)

    viewModel.separationCount = -1
    XCTAssertFalse(viewModel.isSeparationCountValid)
  }
}
