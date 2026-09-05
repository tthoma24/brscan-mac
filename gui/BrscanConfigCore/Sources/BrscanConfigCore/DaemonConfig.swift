/// A typed view of the daemon's configuration (`daemon/config.h`'s `Config`
/// struct / `daemon/config.cpp`'s `ParseConfig`/`ApplyKey`), layered on top
/// of the format-only, key-agnostic `ConfigDocument` (Task 1e.2) and the
/// option-set vocabulary (Task 1e.3: `OptionValueSets`, `Dpi`, `Separation`).
///
/// `ConfigDocument` knows nothing about which keys are valid or what they
/// mean; `DaemonConfig` is the layer that does, so the GUI can bind form
/// fields to `DaemonConfig`'s properties instead of raw strings, while still
/// reading and writing through `ConfigDocument` so comments, key order, and
/// unrecognized keys survive untouched (see `from(_:)`/`apply(to:)` below).
///
/// Every key spelling and default here is taken from `daemon/config.h` and
/// `daemon/config.cpp` (see especially `ParseConfig`'s doc comment, `Config`
/// struct, `DefaultConfig()`, and `ApplyKey`) plus
/// `config/brscan-scand.conf.example` -- not invented independently.
public struct DaemonConfig: Equatable {

  /// The five general (non-per-destination) settings: `daemon/config.h`'s
  /// `Config::printer_host`/`display_name`/`save_dir`/`image_app`/
  /// `email_to`. None of these are validated by `ApplyKey` beyond being
  /// present -- any string is accepted verbatim -- so there is no
  /// "malformed" case for this group, only "missing key -> default".
  public struct General: Equatable {
    /// `printer_host` -- the printer's Bonjour hostname or IP. **Required**:
    /// `daemon/config.h`'s `kDefaultPrinterHost` is `""`, and the daemon
    /// refuses to start with an empty value (see `TryReloadConfig`'s doc
    /// comment) -- there is no safe default to fall back to, since every
    /// device's mDNS name is device-specific.
    public var printerHost: String
    /// `display_name` -- the name shown in the printer's Scan menu. The
    /// daemon's own default (`DefaultDisplayName()`) is this host's
    /// `gethostname()`, which is machine-specific and not something this
    /// editor can reproduce deterministically (nor should it guess). A
    /// missing key is represented here as `""` ("unset" -- the daemon will
    /// fill in its own hostname-derived default at load time), not as a
    /// fabricated hostname.
    public var displayName: String
    /// `save_dir` -- FILE-destination output directory. Stored exactly as
    /// written in the config file (e.g. `"~/Scans"`, not expanded) --
    /// `daemon/config.cpp`'s `ExpandHome` only happens on the daemon side
    /// when the config is loaded, and this editor should not rewrite a
    /// user's `~` into an absolute path. Defaults to `daemon/config.h`'s
    /// `kDefaultSaveDir`, `"~/Scans"`.
    public var saveDir: String
    /// `image_app` -- app name passed to `open -a <image_app>` for the
    /// IMAGE destination. `""` (the default) means no `-a` flag: `open`
    /// picks the file's default app.
    public var imageApp: String
    /// `email_to` -- recipient address a freshly composed EMAIL-destination
    /// message is pre-addressed to. `""` (the default) leaves the To:
    /// field blank.
    public var emailTo: String

    public init(printerHost: String, displayName: String, saveDir: String, imageApp: String, emailTo: String) {
      self.printerHost = printerHost
      self.displayName = displayName
      self.saveDir = saveDir
      self.imageApp = imageApp
      self.emailTo = emailTo
    }
  }

  /// The seven `<dest>.*` settings shared by all four FUNCs (FILE, IMAGE,
  /// OCR, EMAIL) -- `daemon/config.cpp`'s `ParamsForDestPrefix`/
  /// `OutputForDestPrefix`/`PaperForDestPrefix` all key on the same four
  /// `dest` prefixes (`file`, `image`, `ocr`, `email`), each reading the
  /// same seven fields. OCR's output being specialized to a searchable PDF
  /// is a later, UI-side concern (see `config/brscan-scand.conf.example`'s
  /// "OCR is special" note) -- it is not a difference in the schema this
  /// type models, so one `Route` type covers all four destinations.
  public struct Route: Equatable {
    /// `<dest>.mode` -- `color | gray | bw | errdiff | truegray`
    /// (`OptionValueSets.mode`). An unrecognized token, like a missing key,
    /// takes the default (`daemon/config.cpp`'s `ParseModeString` returns
    /// `nullopt` and `ApplyKey` leaves the field unchanged, i.e. at
    /// whichever default `brscan::Params`'s constructor set).
    public var mode: String
    /// `<dest>.source` -- `flatbed | adf | adf-duplex`
    /// (`OptionValueSets.source`). This single token carries both the
    /// daemon's `Source` enum and its `duplex` bool (`adf-duplex` is ADF +
    /// duplex), so one string field is enough to round-trip the key
    /// exactly. Unrecognized/missing -> default.
    public var source: String
    /// `<dest>.dpi` -- a positive integer, sets both `x_dpi` and `y_dpi`
    /// (`Dpi`/`PositiveInt`). Unparsable (non-numeric, zero, negative) or
    /// missing -> default (`GeneratedOptionSets.OptionSets.dpiDefault`,
    /// 300).
    public var dpi: Int
    /// `<dest>.format` -- `pdf | tiff | jpeg | png | native`
    /// (`OptionValueSets.format`). Unrecognized/missing -> default
    /// (`native`, `OutputFormat::kNative`).
    public var format: String
    /// `<dest>.tiff_compression` -- `lzw | g3 | g4`
    /// (`OptionValueSets.tiffCompression`). Only meaningful when
    /// `format == "tiff"` (see `OptionRules.compressionApplies(to:)`).
    /// Unrecognized/missing -> default (`lzw`, `TiffCompression::kLzw`).
    public var tiffCompression: String
    /// `<dest>.separation` -- `combine | off | image:N | page:N` (`every:N`
    /// also parses in as a backward-compat alias for `image:N`; see
    /// `Separation`/`SeparationCodec`). Unparsable/missing -> default
    /// (`.combine`, `OutputSeparation::kCombine`).
    public var separation: Separation
    /// `<dest>.paper` -- one of `OptionValueSets.paper`'s nine tokens, or
    /// `""` for "no explicit paper". Unlike every other `<dest>.*` field
    /// above, `daemon/config.cpp`'s `ApplyKey` does **not** validate this
    /// one: the `field == "paper"` branch stores `value` unconditionally,
    /// with the comment "Stored as-is, with no validation against
    /// daemon/paper_size.h's table". So a config file with an
    /// out-of-vocabulary `<dest>.paper` value is not "malformed" to the
    /// daemon -- it is stored and (elsewhere) simply treated as an unknown
    /// paper. This type mirrors that exactly: any present value, valid
    /// token or not, is kept verbatim; only a missing key falls back to the
    /// default `""`. `OptionValueSets.paper` is still there for a GUI
    /// picker to validate against before offering a token, per Task 1e.3.
    public var paper: String

    public init(
      mode: String, source: String, dpi: Int, format: String, tiffCompression: String, separation: Separation,
      paper: String
    ) {
      self.mode = mode
      self.source = source
      self.dpi = dpi
      self.format = format
      self.tiffCompression = tiffCompression
      self.separation = separation
      self.paper = paper
    }
  }

  public var general: General
  /// FILE destination (`daemon/config.h`'s `kFuncFile`).
  public var file: Route
  /// IMAGE destination (`kFuncImage`).
  public var image: Route
  /// OCR destination (`kFuncOcr`).
  public var ocr: Route
  /// EMAIL destination (`kFuncEmail`).
  public var email: Route

  public init(general: General, file: Route, image: Route, ocr: Route, email: Route) {
    self.general = general
    self.file = file
    self.image = image
    self.ocr = ocr
    self.email = email
  }

  // MARK: Key spellings

  /// The exact `<dest>` prefixes `daemon/config.cpp`'s `ApplyKey` switches
  /// on (`ParamsForDestPrefix`/`OutputForDestPrefix`/`PaperForDestPrefix`).
  private enum Dest {
    static let file = "file"
    static let image = "image"
    static let ocr = "ocr"
    static let email = "email"
  }

  // MARK: Defaults

  /// Every field at the value `daemon/config.cpp`'s `DefaultConfig()` uses
  /// for a config file that sets nothing, except `general.printerHost` and
  /// `general.displayName` -- see `General`'s doc comments for why those
  /// two are `""` here rather than a guessed value.
  public static let `default` = DaemonConfig(
    general: General(printerHost: "", displayName: "", saveDir: "~/Scans", imageApp: "", emailTo: ""),
    file: .default,
    image: .default,
    ocr: .default,
    email: .default)

  // MARK: Reading (ConfigDocument -> DaemonConfig)

  /// Builds a `DaemonConfig` by reading each key this type owns out of
  /// `doc`. A missing key takes the default; a key present but unparsable
  /// for its field (e.g. `file.dpi=abc`) also takes the default, mirroring
  /// `ApplyKey`'s "ignore and leave at prior/default value" behavior --
  /// except `<dest>.paper`, which is never validated (see `Route.paper`).
  ///
  /// One known simplification versus the daemon's own line-by-line fold:
  /// `ConfigDocument.value(for:)` returns the *last active line's raw
  /// value* for a key, regardless of whether that value parses. The
  /// daemon's `ApplyKey`, by contrast, no-ops on an unparsable line,
  /// leaving whatever the previous (valid) line already set. The two only
  /// disagree when a single key has more than one active line and a later
  /// one is invalid -- an unusual, hand-edited config this editor does not
  /// need to reproduce byte-for-byte; `ConfigDocument`'s public API has no
  /// way to observe the daemon's full fold order in this case either.
  public static func from(_ doc: ConfigDocument) -> DaemonConfig {
    DaemonConfig(
      general: General.from(doc),
      file: Route.from(doc, dest: Dest.file),
      image: Route.from(doc, dest: Dest.image),
      ocr: Route.from(doc, dest: Dest.ocr),
      email: Route.from(doc, dest: Dest.email))
  }

  // MARK: Writing (DaemonConfig -> ConfigDocument)

  /// Writes every key this type owns into `doc` via `ConfigDocument.
  /// setValue(_:for:)`, which updates an existing active line in place or
  /// appends a new one -- so comments, blank lines, key order, and any key
  /// this type does not own are left byte-for-byte untouched (Task 1e.2's
  /// round-trip guarantee). This never rewrites the whole file, and never
  /// touches a key outside the fixed set enumerated by `General`/`Route`
  /// above.
  public func apply(to doc: inout ConfigDocument) {
    general.apply(to: &doc)
    file.apply(to: &doc, dest: Dest.file)
    image.apply(to: &doc, dest: Dest.image)
    ocr.apply(to: &doc, dest: Dest.ocr)
    email.apply(to: &doc, dest: Dest.email)
  }
}

extension DaemonConfig.General {
  fileprivate static func from(_ doc: ConfigDocument) -> DaemonConfig.General {
    let defaults = DaemonConfig.default.general
    return DaemonConfig.General(
      printerHost: doc.value(for: "printer_host") ?? defaults.printerHost,
      displayName: doc.value(for: "display_name") ?? defaults.displayName,
      saveDir: doc.value(for: "save_dir") ?? defaults.saveDir,
      imageApp: doc.value(for: "image_app") ?? defaults.imageApp,
      emailTo: doc.value(for: "email_to") ?? defaults.emailTo)
  }

  fileprivate func apply(to doc: inout ConfigDocument) {
    doc.setValue(printerHost, for: "printer_host")
    doc.setValue(displayName, for: "display_name")
    doc.setValue(saveDir, for: "save_dir")
    doc.setValue(imageApp, for: "image_app")
    doc.setValue(emailTo, for: "email_to")
  }
}

extension DaemonConfig.Route {
  /// `brscan::Params`'s own default constructor (color / flatbed / 300dpi)
  /// and `OutputSettings`'s own default constructor (native / lzw /
  /// combine), which `daemon/config.h`'s doc comments call out as this
  /// project's chosen default for every FUNC absent a config override; see
  /// `daemon/config.h`'s `Config::file_params`/`file_output` comments and
  /// `libbrscan/include/brscan/types.h`'s `Params`/`daemon/output_writer.h`'s
  /// `OutputSettings`.
  public static let `default` = DaemonConfig.Route(
    mode: OptionSets.mode[0],  // "color"
    source: OptionSets.source[0],  // "flatbed"
    dpi: OptionSets.dpiDefault,  // 300
    format: OptionSets.format[0],  // "native"
    tiffCompression: OptionSets.tiffCompression[0],  // "lzw"
    separation: .combine,
    paper: "")

  fileprivate static func from(_ doc: ConfigDocument, dest: String) -> DaemonConfig.Route {
    var route = DaemonConfig.Route.default

    if let raw = doc.value(for: "\(dest).mode"), OptionValueSets.mode.isValid(raw) {
      route.mode = raw
    }
    if let raw = doc.value(for: "\(dest).source"), OptionValueSets.source.isValid(raw) {
      route.source = raw
    }
    if let raw = doc.value(for: "\(dest).dpi"), let dpi = Dpi.parse(raw) {
      route.dpi = dpi
    }
    if let raw = doc.value(for: "\(dest).format"), OptionValueSets.format.isValid(raw) {
      route.format = raw
    }
    if let raw = doc.value(for: "\(dest).tiff_compression"), OptionValueSets.tiffCompression.isValid(raw) {
      route.tiffCompression = raw
    }
    if let raw = doc.value(for: "\(dest).separation"), let separation = SeparationCodec.parse(raw) {
      route.separation = separation
    }
    // <dest>.paper: stored as-is, unvalidated -- see Route.paper's doc
    // comment. A present key (any value, including "") always wins over
    // the default; only a fully absent key keeps the default "".
    if let raw = doc.value(for: "\(dest).paper") {
      route.paper = raw
    }

    return route
  }

  fileprivate func apply(to doc: inout ConfigDocument, dest: String) {
    doc.setValue(mode, for: "\(dest).mode")
    doc.setValue(source, for: "\(dest).source")
    doc.setValue(String(dpi), for: "\(dest).dpi")
    doc.setValue(format, for: "\(dest).format")
    doc.setValue(tiffCompression, for: "\(dest).tiff_compression")
    doc.setValue(SeparationCodec.serialize(separation), for: "\(dest).separation")
    doc.setValue(paper, for: "\(dest).paper")
  }
}
