// GENERATED FILE -- DO NOT EDIT BY HAND.
//
// Regenerate with `python3 scripts/gen-option-sets.py` from the repo root.
// Source of truth: config/option-sets.json -- see that file for exactly
// which daemon parser (daemon/config.cpp, daemon/paper_size.h) each token
// set mirrors. A CTest entry (see CMakeLists.txt's OptionSetsUpToDate)
// regenerates this file into a temp location and diffs it against this
// committed copy, so an edit to option-sets.json without a matching
// `gen-option-sets.py` run fails CI.

/// The valid `<dest>.<key>` config tokens the daemon (daemon/config.cpp)
/// accepts, and the paper tokens daemon/paper_size.h's `IsKnownPaper`
/// recognizes -- generated from config/option-sets.json so the GUI can
/// never offer a value the daemon would silently ignore.
public enum OptionSets {
  /// `<dest>.mode` tokens (daemon/config.cpp's ParseModeString).
  public static let mode: [String] = ["color", "gray", "bw", "errdiff", "truegray"]

  /// `<dest>.source` tokens (daemon/config.cpp's ParseSourceString).
  public static let source: [String] = ["flatbed", "adf", "adf-duplex"]

  /// `<dest>.format` tokens (daemon/config.cpp's ParseFormatString).
  public static let format: [String] = ["native", "pdf", "tiff", "jpeg", "png"]

  /// `<dest>.tiff_compression` tokens (daemon/config.cpp's ParseTiffCompressionString).
  public static let tiffCompression: [String] = ["lzw", "g3", "g4"]

  /// `<dest>.separation` *modes* -- combine/off take no suffix; image/page each take a ':N' positive-int suffix (e.g. "image:3"). "every:N" is also accepted by the daemon as a backward-compat alias for "image:N" (same mode, not a separate one), so it isn't listed here.
  public static let separationMode: [String] = ["combine", "off", "image", "page"]

  /// `<dest>.paper` tokens (daemon/paper_size.h's IsKnownPaper / kPaperTable).
  public static let paper: [String] = ["LETTER", "LEGAL", "A4", "LEDGER", "A3", "A5", "EXECUTIVE", "PHOTO", "BCARD"]

  /// `<dest>.dpi` is not an enumerated set: any positive integer is
  /// accepted by daemon/config.cpp's ParsePositiveInt. This is the
  /// documented default (brscan::Params's own default constructor).
  public static let dpiDefault: Int = 300
}
