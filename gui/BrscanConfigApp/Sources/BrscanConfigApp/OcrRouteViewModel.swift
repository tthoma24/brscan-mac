import BrscanConfigCore
import Foundation

/// View model for the **OCR** tab: a thin specialization of `RouteViewModel`
/// (task 1e.7) for how the daemon treats the OCR route. OCR has its own
/// output sub-format vocabulary -- `pdf` (a searchable, Vision-recognized
/// text layer over the scanned image) or one of the `txt`/`html`/`rtf` text
/// sinks -- honored via the daemon's `ocr.ocr_format` key
/// (`daemon/config.cpp`'s `ParseOcrFormatString`, Task 1e.14), *not* the
/// image `<dest>.format` vocabulary the other route tabs pick from. So this
/// type's `format` picker binds to `OptionValueSets.ocrFormat`, and there is
/// no `tiffCompression` control (compression is a TIFF-image concern, never
/// relevant to an OCR PDF or text sink).
///
/// This duplicates `RouteViewModel`'s non-`ocrFormat` scan-param fields
/// (mode/source/dpi/paper/separation) rather than wrapping a `RouteViewModel`
/// instance and forwarding its `ObservableObject` publisher just to swap one
/// field -- of the two, the smaller change. The one piece of shared state,
/// `SeparationMode`, is reused directly from `RouteViewModel` rather than
/// redeclared.
public final class OcrRouteViewModel: ObservableObject {
  @Published public var mode: String
  @Published public var source: String
  @Published public var dpi: Int
  @Published public var paper: String

  /// `ocr.high_speed` -- high-speed ADF mode: the feeder scans landscape at
  /// full speed and the daemon rotates each page back to portrait (Task
  /// 1e.16). A plain on/off toggle, same as `RouteViewModel.highSpeed`.
  @Published public var highSpeed: Bool
  /// `ocr.skip_blank` -- drop blank pages from ADF output (Task 1e.18). A
  /// plain on/off toggle, same as `RouteViewModel.skipBlank`.
  @Published public var skipBlank: Bool

  /// The OCR output sub-format: `pdf` (searchable PDF) or one of the
  /// `txt`/`html`/`rtf` text sinks, backed by `OptionValueSets.ocrFormat`
  /// (the daemon's `ocr.ocr_format` key). A real, LCD-independent choice the
  /// daemon honors (Task 1e.14) -- so unlike the pre-1e.14 OCR tab, which
  /// pinned this to a fixed searchable PDF, `OcrTabView` offers a picker
  /// bound to this field.
  @Published public var format: String

  /// See `RouteViewModel.separationMode`/`separationCount`'s doc comment --
  /// same reasoning, reused type.
  @Published public var separationMode: RouteViewModel.SeparationMode
  @Published public var separationCount: Int

  /// The fixed image-container `<dest>.format` value written for the OCR
  /// route. The daemon ignores `ocr.format` for OCR (it keys the output off
  /// `ocr.ocr_format` instead -- see `Config::ocr_text_format`), so this is
  /// pinned rather than exposed; the user-facing choice is `format` above,
  /// the OCR sub-format.
  private static let imageContainerFormat = "pdf"

  public init(route: DaemonConfig.Route = DaemonConfig.Route.default) {
    self.mode = route.mode
    self.source = route.source
    self.dpi = route.dpi
    self.paper = route.paper
    self.format = route.ocrFormat
    self.highSpeed = route.highSpeed
    self.skipBlank = route.skipBlank

    switch route.separation {
    case .combine:
      self.separationMode = .combine
      self.separationCount = 1
    case .image(let n):
      self.separationMode = .image
      self.separationCount = n
    case .page(let n):
      self.separationMode = .page
      self.separationCount = n
    }
  }

  // MARK: Gating (reuses `OptionRules`, task 1e.3)

  /// `<dest>.separation` is editable only when the OCR sub-format is a
  /// container that can hold more than one page per file -- `pdf`. The text
  /// sinks (`txt`/`html`/`rtf`) aren't containers, so separation doesn't
  /// apply. Computed the same way `RouteViewModel.isSeparationEditable` is,
  /// via `OptionRules.separationApplies(to:)`, so a future format-gating
  /// change there still applies here without revisiting this type.
  public var isSeparationEditable: Bool {
    OptionRules.separationApplies(to: format)
  }

  /// Whether the "searchable PDF" explanatory note applies to the current
  /// selection -- true only for the `pdf` sub-format, the one that carries a
  /// Vision text layer (`OptionRules.searchableApplies(to:)`). The text
  /// sinks produce recognized text directly, so the note is hidden for them.
  public var isSearchablePdf: Bool {
    OptionRules.searchableApplies(to: format)
  }

  // MARK: Validation

  /// Whether `dpi` is a value the daemon would accept. Reuses `Dpi.
  /// isValid(_:)` (task 1e.3), same as `RouteViewModel.isDpiValid`.
  public var isDpiValid: Bool {
    Dpi.isValid(dpi)
  }

  /// Whether `separationCount` is a value the daemon would accept. Reuses
  /// `Dpi.isValid(_:)`, same as `RouteViewModel.isSeparationCountValid`.
  public var isSeparationCountValid: Bool {
    Dpi.isValid(separationCount)
  }

  // MARK: Assembly

  /// Reassembles `separationMode`/`separationCount` into the `Separation`
  /// value `route` below serializes via `SeparationCodec`.
  public var separation: Separation {
    switch separationMode {
    case .combine: return .combine
    case .image: return .image(separationCount)
    case .page: return .page(separationCount)
    }
  }

  /// Reassembles every bound field into a `DaemonConfig.Route`. The OCR
  /// sub-format goes to `ocrFormat` (the daemon's `ocr.ocr_format` key); the
  /// image `format` field is pinned to `imageContainerFormat` since the
  /// daemon ignores `ocr.format` for OCR, and `tiffCompression` is left at
  /// its schema default (compression never applies to OCR output). A
  /// hand-edited image `ocr.format=tiff` in the config file, which the
  /// daemon ignores for OCR, is therefore not perpetuated on save.
  public var route: DaemonConfig.Route {
    DaemonConfig.Route(
      mode: mode,
      source: source,
      dpi: dpi,
      format: Self.imageContainerFormat,
      tiffCompression: DaemonConfig.Route.default.tiffCompression,
      separation: separation,
      paper: paper,
      ocrFormat: format,
      highSpeed: highSpeed,
      skipBlank: skipBlank)
  }

  // MARK: Reseeding

  /// Resets every bound field to `route`'s values, in place (`format` seeded
  /// from `route.ocrFormat`) -- unlike `init(route:)`, this doesn't replace
  /// the view model instance, so `ConfigStore` (task 1e.9) can reseed an
  /// already-created, already-bound `OcrRouteViewModel` after loading a
  /// config file from disk.
  public func load(_ route: DaemonConfig.Route) {
    mode = route.mode
    source = route.source
    dpi = route.dpi
    paper = route.paper
    format = route.ocrFormat
    highSpeed = route.highSpeed
    skipBlank = route.skipBlank

    switch route.separation {
    case .combine:
      separationMode = .combine
      separationCount = 1
    case .image(let n):
      separationMode = .image
      separationCount = n
    case .page(let n):
      separationMode = .page
      separationCount = n
    }
  }

  /// The OCR tab's info note, named out as a constant rather than a string
  /// literal inline in `OcrTabView` so `OcrRouteViewModelTests` can pin its
  /// key claims (Google Developer Style Guide sentence case) against the
  /// exact same text the view renders -- one text ever edited, one place a
  /// mismatch would be caught.
  public enum Notes {
    /// Shown only when the `pdf` sub-format is selected (see
    /// `isSearchablePdf`): the PDF output carries a Vision-recognized text
    /// layer. The text sinks produce recognized text directly, so this note
    /// doesn't apply to them.
    public static let searchablePdf =
      "Searchable PDF: a Vision-recognized, invisible text layer is added over the scanned image."
  }
}
