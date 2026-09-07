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

  /// `<dest>.jpeg_quality` (the Brother driver's "File Size" control) only
  /// affects output when `format == "jpeg"` -- the daemon writes a JPEG's
  /// bytes at that quality; every other format ignores it.
  public static func jpegQualityApplies(to format: String) -> Bool {
    format == "jpeg"
  }
}
