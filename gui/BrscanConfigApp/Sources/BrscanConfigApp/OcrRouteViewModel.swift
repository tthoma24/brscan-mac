import BrscanConfigCore
import Foundation

/// View model for the **OCR** tab: a thin specialization of `RouteViewModel`
/// (task 1e.7) for how the daemon actually treats the OCR route.
/// `daemon/button_plan.cpp`/`daemon/handle_event.cpp` always produce a
/// searchable PDF (a Vision-recognized text layer) for OCR, regardless of
/// the configured/LCD-selected output type -- the LCD's OCR sub-formats
/// (`T=TXT`/`HTML`/`RTF`) are parsed but not yet produced (see
/// `docs/BUTTON.md`'s "OCR always yields a searchable PDF" notes). So unlike
/// `RouteViewModel`, this type has no `format` or `tiffCompression`
/// published property for a picker to bind to: `format` is a fixed
/// constant, never a free choice.
///
/// This duplicates `RouteViewModel`'s five non-format scan-param fields
/// (mode/source/dpi/paper/separation) rather than wrapping a `RouteViewModel`
/// instance and forwarding its `ObservableObject` publisher just to hide one
/// field -- of the two, the smaller change: it makes "OCR can never produce
/// a non-PDF format" a property of this type's shape (there is no settable
/// `format` to misuse), not a runtime override a future edit could bypass.
/// The one piece of shared state, `SeparationMode`, is reused directly from
/// `RouteViewModel` rather than redeclared.
public final class OcrRouteViewModel: ObservableObject {
  @Published public var mode: String
  @Published public var source: String
  @Published public var dpi: Int
  @Published public var paper: String

  /// See `RouteViewModel.separationMode`/`separationCount`'s doc comment --
  /// same reasoning, reused type.
  @Published public var separationMode: RouteViewModel.SeparationMode
  @Published public var separationCount: Int

  /// The fixed output format for OCR: always a searchable PDF (see this
  /// type's doc comment). Not `@Published` and has no setter -- there is no
  /// format choice to offer, so `RouteEditorView`'s format picker has no
  /// counterpart here; `OcrTabView` shows this as a plain, non-editable row
  /// instead.
  public let format = "pdf"

  public init(route: DaemonConfig.Route = DaemonConfig.Route.default) {
    self.mode = route.mode
    self.source = route.source
    self.dpi = route.dpi
    self.paper = route.paper

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

  /// `<dest>.separation` is always editable for OCR: its output format is
  /// always `pdf`, one of the container formats
  /// `OptionRules.separationApplies(to:)` allows. Computed the same way
  /// `RouteViewModel.isSeparationEditable` is, just against the one format
  /// OCR ever has, so a future format-gating change in `OptionRules` still
  /// applies here without this type needing to be revisited.
  public var isSeparationEditable: Bool {
    OptionRules.separationApplies(to: format)
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

  /// Reassembles every bound field into a `DaemonConfig.Route`, with
  /// `format` always `"pdf"` and `tiffCompression` left at its schema
  /// default (irrelevant for pdf output --
  /// `OptionRules.compressionApplies(to: "pdf")` is always false, so no
  /// value here changes daemon behavior). This can never produce a non-PDF
  /// format, regardless of what the seed route's `format` was -- e.g. a
  /// hand-edited `ocr.format=tiff` in the config file, which the daemon
  /// ignores for OCR today (`docs/BUTTON.md`), so this editor should not
  /// perpetuate it on save.
  public var route: DaemonConfig.Route {
    DaemonConfig.Route(
      mode: mode,
      source: source,
      dpi: dpi,
      format: format,
      tiffCompression: DaemonConfig.Route.default.tiffCompression,
      separation: separation,
      paper: paper)
  }

  // MARK: Reseeding

  /// Resets every bound field to `route`'s values, in place (`format` is
  /// always fixed, so it's never touched) -- unlike `init(route:)`, this
  /// doesn't replace the view model instance, so `ConfigStore` (task 1e.9)
  /// can reseed an already-created, already-bound `OcrRouteViewModel` after
  /// loading a config file from disk.
  public func load(_ route: DaemonConfig.Route) {
    mode = route.mode
    source = route.source
    dpi = route.dpi
    paper = route.paper

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

  /// The OCR tab's info notes, named out as constants rather than string
  /// literals inline in `OcrTabView` so `OcrRouteViewModelTests` can pin
  /// their key claims (Google Developer Style Guide sentence case; accurate
  /// per `docs/BUTTON.md`) against the exact same text the view renders --
  /// one text ever edited, one place a mismatch would be caught.
  public enum Notes {
    /// OCR always yields a searchable PDF -- the daemon-side behavior this
    /// whole view model exists to reflect (see the type's doc comment).
    public static let searchablePdf =
      "OCR output is always a searchable PDF: a Vision-recognized, invisible text layer over the scanned image."

    /// The LCD's OCR sub-formats (`T=TXT`/`HTML`/`RTF`) are parsed by the
    /// daemon but not yet produced -- see `docs/BUTTON.md`'s "Not yet
    /// implemented" note.
    public static let subFormatsNotProduced =
      "The printer's OCR file-type choice (Text, HTML, or RTF on the LCD) isn't produced by this daemon yet -- OCR always yields a searchable PDF."
  }
}
