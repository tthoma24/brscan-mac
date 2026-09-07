/// Typed value sets for the `<dest>.*` config keys that are a closed
/// enumeration of tokens (as opposed to `dpi`, which is an open positive
/// integer -- see `Dpi.swift` -- or `separation`, which has its own `:N`
/// grammar -- see `Separation.swift`).
///
/// Each value set is a thin wrapper around the corresponding array in
/// `GeneratedOptionSets.swift` (Task 1e.1, generated from
/// `config/option-sets.json`): `options` is never a hand-copied literal
/// list, so a token the daemon starts/stops accepting can't silently drift
/// out of sync between this file and the generated constants -- adding a
/// raw token literal here that isn't in `OptionSets` is exactly the drift
/// this type is meant to prevent.
public struct OptionValueSet: Equatable {
  /// The valid tokens for this key, straight from `GeneratedOptionSets`.
  public let options: [String]

  init(options: [String]) {
    self.options = options
  }

  /// Whether `token` is one of `options`. Case-sensitive, matching the
  /// daemon's own parsers (`daemon/config.cpp`'s `Parse*String` helpers,
  /// `daemon/paper_size.h`'s `IsKnownPaper`).
  public func isValid(_ token: String) -> Bool {
    options.contains(token)
  }
}

/// The closed-enumeration `<dest>.*` value sets, one per key.
public enum OptionValueSets {
  /// `<dest>.mode` -- daemon/config.cpp's `ParseModeString`.
  public static let mode = OptionValueSet(options: OptionSets.mode)

  /// `<dest>.source` -- daemon/config.cpp's `ParseSourceString`.
  public static let source = OptionValueSet(options: OptionSets.source)

  /// `<dest>.format` -- daemon/config.cpp's `ParseFormatString`.
  public static let format = OptionValueSet(options: OptionSets.format)

  /// `<dest>.tiff_compression` -- daemon/config.cpp's
  /// `ParseTiffCompressionString`. Only meaningful when `format == "tiff"`
  /// -- see `OptionRules.compressionApplies(to:)`.
  public static let tiffCompression = OptionValueSet(options: OptionSets.tiffCompression)

  /// `<dest>.paper` -- daemon/paper_size.h's `IsKnownPaper`/`kPaperTable`.
  public static let paper = OptionValueSet(options: OptionSets.paper)

  /// `ocr.ocr_format` -- daemon/config.cpp's `ParseOcrFormatString`. OCR-only
  /// (honored solely under the `ocr` dest prefix): the OCR route's output
  /// sub-format, `pdf` (searchable PDF) or one of the `txt`/`html`/`rtf`
  /// text sinks. A distinct vocabulary from `format` above.
  public static let ocrFormat = OptionValueSet(options: OptionSets.ocrFormat)
}
