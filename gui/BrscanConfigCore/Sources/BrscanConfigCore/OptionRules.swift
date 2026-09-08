/// Format-gating rules: which `<dest>.*` keys are meaningful for a given
/// `<dest>.format` value. These mirror behavior the daemon itself already
/// encodes (`output/output_writer.h`'s `OutputFormat`/`TiffCompression`/
/// `OutputSettings.searchable`) so the GUI can gray out or hide a control
/// the daemon would silently ignore, without duplicating the *set* of
/// format tokens -- `format` here is always one of `OptionValueSets.format`'s
/// `options` (i.e. `GeneratedOptionSets.OptionSets.format`).
public enum OptionRules {
  /// `<dest>.tiff_compression` only affects output when `format == "tiff"`
  /// (output/output_writer.h's `TiffCompression` is a `tiff`-only setting).
  public static func compressionApplies(to format: String) -> Bool {
    format == "tiff"
  }

  /// `<dest>.separation` (splitting output into multiple files by image or
  /// page count) only applies to the container formats that can hold more
  /// than one page per file, `pdf` and `tiff`. The per-page formats
  /// (`jpeg`, `png`, `native`) always write one file per page already, so
  /// separation is not applicable -- treat it as `combine`.
  public static func separationApplies(to format: String) -> Bool {
    format == "pdf" || format == "tiff"
  }

  /// `searchable` (a Vision OCR text layer) only applies to `pdf` output
  /// (output/output_writer.h's `OutputSettings.searchable` doc comment:
  /// "PDF only").
  public static func searchableApplies(to format: String) -> Bool {
    format == "pdf"
  }

  /// `<dest>.jpeg_quality` (the Brother driver's "File Size" control) affects
  /// output for the lossy image formats -- `jpeg`, `heic`, and `jp2`. The
  /// daemon encodes each of those at that quality (output/output_writer.mm's
  /// `kJpeg`/`kHeic`/`kJpeg2000` all pass `settings.jpeg_quality`); every
  /// other format (the lossless `png`/`gif`/`bmp`, the multi-page `pdf`/
  /// `tiff`, and `native`) ignores it.
  public static func jpegQualityApplies(to format: String) -> Bool {
    format == "jpeg" || format == "heic" || format == "jp2"
  }

  /// The `<dest>.format` values that make sense for a given `<dest>.mode`,
  /// as a subset of `OptionValueSets.format`'s `options` (order preserved).
  /// This mirrors the vendor: Image Capture gates its Format menu by pixel
  /// type, but the daemon button-flow path doesn't, so the GUI hides a
  /// format a mode can't sensibly produce instead of letting the daemon
  /// silently transcode it (e.g. a B&W route to color HEIC).
  ///
  /// The mode maps to a pixel class, which the encoders constrain:
  /// - `color` -> RGB: every format is allowed.
  /// - `gray`, `truegray` -> grayscale: every format except `heic` (HEVC
  ///   rejects single-channel input).
  /// - `bw`, `errdiff` -> bitonal (1-bit): every format except the lossy
  ///   photo formats `heic`, `jpeg`, and `jp2` (which need >= 8-bit).
  ///
  /// An unrecognized mode token defaults to the most permissive (color)
  /// set, so nothing is wrongly hidden.
  public static func allowedFormats(forMode mode: String) -> [String] {
    let excluded: Set<String>
    switch mode {
    case "gray", "truegray":
      excluded = ["heic"]
    case "bw", "errdiff":
      excluded = ["heic", "jpeg", "jp2"]
    default:  // "color", or any unrecognized mode -> allow every format.
      excluded = []
    }
    return OptionValueSets.format.options.filter { !excluded.contains($0) }
  }
}
