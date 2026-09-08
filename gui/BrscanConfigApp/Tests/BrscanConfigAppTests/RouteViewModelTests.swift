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

  // MARK: Format coercion on mode change

  /// Changing to a mode that can't produce the current format coerces the
  /// format to `pdf`, so the produced route never pairs an invalid
  /// mode+format (e.g. bw + heic, which the daemon would transcode).
  func testChangingModeToBwCoercesInvalidHeicFormatToPdf() {
    let viewModel = RouteViewModel()
    viewModel.mode = "color"
    viewModel.format = "heic"

    viewModel.mode = "bw"

    XCTAssertEqual(viewModel.format, "pdf")
    XCTAssertEqual(viewModel.route.mode, "bw")
    XCTAssertEqual(viewModel.route.format, "pdf")
  }

  /// Grayscale rejects only heic; a jpeg route survives the switch.
  func testChangingModeToGrayCoercesHeicButKeepsJpeg() {
    let viewModel = RouteViewModel()
    viewModel.mode = "color"
    viewModel.format = "heic"
    viewModel.mode = "gray"
    XCTAssertEqual(viewModel.format, "pdf")

    viewModel.format = "jpeg"
    viewModel.mode = "truegray"
    XCTAssertEqual(viewModel.format, "jpeg")
  }

  /// A format still valid for the new mode is left untouched.
  func testChangingModeLeavesAValidFormatUntouched() {
    let viewModel = RouteViewModel()
    viewModel.format = "tiff"

    viewModel.mode = "bw"
    XCTAssertEqual(viewModel.format, "tiff")

    viewModel.mode = "color"
    XCTAssertEqual(viewModel.format, "tiff")
  }

  /// The produced route is always a valid mode+format pair across every
  /// mode, given a format that some modes disallow.
  func testProducedRouteNeverPairsAnInvalidModeAndFormat() {
    for startFormat in ["heic", "jpeg", "jp2"] {
      let viewModel = RouteViewModel()
      viewModel.format = startFormat
      for mode in OptionSets.mode {
        viewModel.mode = mode
        XCTAssertTrue(
          OptionRules.allowedFormats(forMode: viewModel.route.mode).contains(viewModel.route.format),
          "route \(viewModel.route.mode)+\(viewModel.route.format) should be a valid pair")
      }
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

  // MARK: high_speed / skip_blank toggles (Tasks 1e.16/1e.18)

  /// The toggles default off and, once flipped, are reflected in the
  /// produced route (which `DaemonConfig.Route.apply` serializes to
  /// `<dest>.high_speed = on` / `<dest>.skip_blank = on`).
  func testTogglesDefaultOffAndBindIntoRoute() {
    let viewModel = RouteViewModel()
    XCTAssertFalse(viewModel.highSpeed)
    XCTAssertFalse(viewModel.skipBlank)
    XCTAssertFalse(viewModel.route.highSpeed)
    XCTAssertFalse(viewModel.route.skipBlank)

    viewModel.highSpeed = true
    viewModel.skipBlank = true
    XCTAssertTrue(viewModel.route.highSpeed)
    XCTAssertTrue(viewModel.route.skipBlank)
  }

  /// Seeding from a route with the toggles set populates the fields, and
  /// producing a route round-trips them.
  func testSeedingTogglesRoundTrips() {
    var seed = DaemonConfig.Route.default
    seed.highSpeed = true
    seed.skipBlank = true

    let viewModel = RouteViewModel(route: seed)
    XCTAssertTrue(viewModel.highSpeed)
    XCTAssertTrue(viewModel.skipBlank)
    XCTAssertEqual(viewModel.route, seed)
  }

  /// `load` reseeds the toggles in place, same as `init(route:)`.
  func testLoadReseedsToggles() {
    let viewModel = RouteViewModel()
    var reseed = DaemonConfig.Route.default
    reseed.highSpeed = true
    reseed.skipBlank = true

    viewModel.load(reseed)
    XCTAssertTrue(viewModel.highSpeed)
    XCTAssertTrue(viewModel.skipBlank)
  }

  // MARK: JPEG quality (File size slider)

  /// Defaults to 90 and binds into the produced route.
  func testJpegQualityDefaultsTo90AndBindsIntoRoute() {
    let viewModel = RouteViewModel()
    XCTAssertEqual(viewModel.jpegQuality, 90)
    XCTAssertEqual(viewModel.route.jpegQuality, 90)

    viewModel.jpegQuality = 60
    XCTAssertEqual(viewModel.route.jpegQuality, 60)
  }

  /// The File size slider is surfaced for the lossy image formats (jpeg,
  /// heic, jp2) and hidden for every other format.
  func testJpegQualityEditableForLossyFormats() {
    let lossy: Set<String> = ["jpeg", "heic", "jp2"]
    let viewModel = RouteViewModel()
    for format in OptionSets.format {
      viewModel.format = format
      XCTAssertEqual(
        viewModel.isJpegQualityEditable, lossy.contains(format),
        "jpeg-quality gating mismatch for \(format)")
    }
  }

  /// Seeding from a route with a set quality populates the field, and
  /// producing a route round-trips it. `load` reseeds in place too.
  func testJpegQualitySeedsAndReseeds() {
    var seed = DaemonConfig.Route.default
    seed.format = "jpeg"
    seed.jpegQuality = 100

    let viewModel = RouteViewModel(route: seed)
    XCTAssertEqual(viewModel.jpegQuality, 100)
    XCTAssertEqual(viewModel.route, seed)

    var reseed = DaemonConfig.Route.default
    reseed.jpegQuality = 30
    viewModel.load(reseed)
    XCTAssertEqual(viewModel.jpegQuality, 30)
  }

  /// A HEIC route round-trips `<dest>.jpeg_quality` through the view model:
  /// the slider is editable and the produced route carries the value, since
  /// the daemon encodes HEIC at that quality too.
  func testHeicRouteRoundTripsJpegQuality() {
    var seed = DaemonConfig.Route.default
    seed.format = "heic"
    seed.jpegQuality = 45

    let viewModel = RouteViewModel(route: seed)
    XCTAssertTrue(viewModel.isJpegQualityEditable)
    XCTAssertEqual(viewModel.jpegQuality, 45)
    XCTAssertEqual(viewModel.route, seed)

    viewModel.jpegQuality = 70
    XCTAssertEqual(viewModel.route.jpegQuality, 70)
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
