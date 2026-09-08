import BrscanConfigCore
import Foundation

/// View model for one route tab -- **File**, **Image**, or **Email**
/// (**OCR** gets its own editor in task 1e.8, since it also needs the
/// Vision OCR-only `searchable` field this type does not expose): wraps a
/// `DaemonConfig.Route` and exposes `@Published`, individually bindable
/// properties for its seven `<dest>.*` fields, plus gating flags computed
/// from `OptionRules` (task 1e.3) that `RouteEditorView` uses to enable/
/// disable format-conditional controls, and a computed `route` property
/// that reassembles the edited fields into a `DaemonConfig.Route` for a
/// later save (task 1e.9).
///
/// Mirrors `GeneralViewModel`'s shape (task 1e.6): one `ObservableObject`
/// per config section, seeded from the typed `BrscanConfigCore` struct, no
/// file load/save here and no side-effecting seam needed (unlike
/// `GeneralViewModel`'s `FolderPicking`) -- every field is a plain value
/// edited in place.
public final class RouteViewModel: ObservableObject {
  /// The three separation *modes* the view's picker offers. `combine` and
  /// `off` are the same thing to the daemon (see `Separation.combine`), so
  /// the UI only ever needs to distinguish these three, not the four raw
  /// `OptionSets.separationMode` tokens.
  public enum SeparationMode: String, CaseIterable, Identifiable, Equatable {
    case combine
    case image
    case page

    public var id: String { rawValue }

    /// Sentence-case label per the Google Developer Style Guide.
    public var title: String {
      switch self {
      case .combine: return "Combine into one file"
      case .image: return "Split by image count"
      case .page: return "Split by page count"
      }
    }
  }

  /// The safe format the mode setter falls back to when a mode change makes
  /// the current `format` invalid. `pdf` is in every mode's allowed set (see
  /// `OptionRules.allowedFormats(forMode:)`), so it is always a valid pair.
  private static let safeFormat = "pdf"

  /// `<dest>.mode`. Changing it re-gates the allowed formats
  /// (`OptionRules.allowedFormats(forMode:)`); if the current `format` is no
  /// longer allowed for the new mode, it is coerced to `safeFormat` here --
  /// in the setter, not just the view -- so `route` never emits, and a save
  /// never persists, an invalid mode+format pair.
  @Published public var mode: String {
    didSet { coerceFormatIfInvalid() }
  }
  @Published public var source: String
  @Published public var dpi: Int
  @Published public var format: String
  @Published public var tiffCompression: String
  @Published public var paper: String

  /// `<dest>.high_speed` -- high-speed ADF mode: the feeder scans landscape
  /// at full speed and the daemon rotates each page back to portrait (Task
  /// 1e.16). A plain on/off toggle.
  @Published public var highSpeed: Bool
  /// `<dest>.skip_blank` -- drop blank pages from ADF output (Task 1e.18). A
  /// plain on/off toggle.
  @Published public var skipBlank: Bool

  /// `<dest>.jpeg_quality` -- JPEG output quality, `0...100` (0 = smallest
  /// file, 100 = highest quality), matching the Brother driver's "File Size:
  /// Small ... High Quality" control. A bounded int (see `JpegQuality`),
  /// surfaced only when `format == "jpeg"` (see `isJpegQualityEditable`).
  @Published public var jpegQuality: Int

  /// The separation mode and its `N`, kept as two separate published
  /// fields (rather than one `@Published var separation: Separation`) so
  /// the view can bind a mode picker and an `N` stepper independently.
  /// `separationCount` is kept even while `separationMode == .combine` (it
  /// simply isn't read), so switching the picker to `.image`/`.page` and
  /// back doesn't lose a previously entered `N`.
  @Published public var separationMode: SeparationMode
  @Published public var separationCount: Int

  public init(route: DaemonConfig.Route = DaemonConfig.Route.default) {
    self.mode = route.mode
    self.source = route.source
    self.dpi = route.dpi
    self.format = route.format
    self.tiffCompression = route.tiffCompression
    self.paper = route.paper
    self.highSpeed = route.highSpeed
    self.skipBlank = route.skipBlank
    self.jpegQuality = route.jpegQuality

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

    // `didSet` does not fire during initialization, so seed-time coercion is
    // explicit here -- mirroring `load` -- so a `RouteViewModel` built from an
    // already-invalid pair also normalizes it up front.
    coerceFormatIfInvalid()
  }

  // MARK: Gating (reuses `OptionRules`, task 1e.3 -- never reinvented here)

  /// `<dest>.tiff_compression` is only editable when `format == "tiff"`.
  public var isCompressionEditable: Bool {
    OptionRules.compressionApplies(to: format)
  }

  /// `<dest>.separation` is only editable for the container formats (pdf,
  /// tiff); for the per-page formats (jpeg, png, native) it is not
  /// applicable -- the daemon behaves as if it were always `combine`.
  public var isSeparationEditable: Bool {
    OptionRules.separationApplies(to: format)
  }

  /// `<dest>.jpeg_quality` is only meaningful for the lossy image formats
  /// (`jpeg`, `heic`, `jp2`) -- every other format ignores it (see
  /// `OptionRules.jpegQualityApplies(to:)`), so the view only surfaces the
  /// file-size/quality slider for those.
  public var isJpegQualityEditable: Bool {
    OptionRules.jpegQualityApplies(to: format)
  }

  // MARK: Validation
  //
  // `isDpiValid`/`isSeparationCountValid` no longer gate any inline UI (#120
  // moved dpi/separation-count entry off the route editor); they remain to
  // mirror the daemon's validation for the unit tests that assert the GUI's
  // notion of "valid" matches `ParsePositiveInt`.

  /// Whether `dpi` is a value the daemon would accept: strictly positive.
  /// Reuses `Dpi.isValid(_:)` (task 1e.3) rather than reinventing the
  /// positive-int check.
  public var isDpiValid: Bool {
    Dpi.isValid(dpi)
  }

  /// Whether `separationCount` (the `N` in `image:N`/`page:N`) is a value
  /// the daemon would accept. `Dpi.isValid(_:)`'s check -- strictly
  /// positive -- is exactly `daemon/config.cpp`'s `ParsePositiveInt`
  /// requirement for this `N` too (see `Separation.swift`'s doc comment:
  /// both dpi and separation's `N` share the same `PositiveInt` parser), so
  /// it's reused here rather than duplicated.
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

  /// Reassembles every bound field into a `DaemonConfig.Route`, reflecting
  /// whatever edits have been made. A later save (task 1e.9) serializes
  /// this via `DaemonConfig.Route.apply(to:dest:)`.
  public var route: DaemonConfig.Route {
    DaemonConfig.Route(
      mode: mode,
      source: source,
      dpi: dpi,
      format: format,
      tiffCompression: tiffCompression,
      separation: separation,
      paper: paper,
      highSpeed: highSpeed,
      skipBlank: skipBlank,
      jpegQuality: jpegQuality)
  }

  // MARK: Reseeding

  /// Resets every bound field to `route`'s values, in place -- unlike
  /// `init(route:)`, this doesn't replace the view model instance, so
  /// `ConfigStore` (task 1e.9) can reseed an already-created, already-bound
  /// `RouteViewModel` after loading a config file from disk.
  public func load(_ route: DaemonConfig.Route) {
    mode = route.mode
    source = route.source
    dpi = route.dpi
    format = route.format
    tiffCompression = route.tiffCompression
    paper = route.paper
    highSpeed = route.highSpeed
    skipBlank = route.skipBlank
    jpegQuality = route.jpegQuality

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

    // The load path seeds `mode` then `format` explicitly, so the `mode`
    // setter's coercion (which fired against the *old* format) can't catch a
    // loaded pair that is already invalid -- e.g. a hand-edited `bw` + `heic`.
    // Coerce once more here, after both are seeded, so an invalid loaded pair
    // is normalized immediately and never re-persisted.
    coerceFormatIfInvalid()
  }

  /// Coerces `format` to `safeFormat` when it isn't in the current `mode`'s
  /// allowed set. Shared by the `mode` setter (a user mode change) and `load`
  /// (a freshly seeded pair) so both normalize an invalid mode+format pair
  /// the same way. A pair that is already valid is left untouched.
  private func coerceFormatIfInvalid() {
    if !OptionRules.allowedFormats(forMode: mode).contains(format) {
      format = Self.safeFormat
    }
  }
}
