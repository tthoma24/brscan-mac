/// `<dest>.jpeg_quality` isn't an enumerated set like `OptionValueSets`'
/// members -- `daemon/config.cpp` parses it as an integer and clamps it to
/// `0...100` (0 = smallest file, 100 = highest quality), so this validates
/// the *shape*/range of the value instead of membership in a token list.
/// Mirrors `Dpi`'s shape (a bounded int, not a token set). The default,
/// applied when the key is absent or unparsable, is 90.
public enum JpegQuality {
  /// The daemon's default JPEG quality when the key is absent or unparsable.
  public static let `default` = 90

  /// The inclusive range the daemon accepts (and clamps to): `0...100`.
  public static let range = 0...100

  /// Whether `value` is a JPEG quality the daemon would keep as-is: within
  /// `0...100`. A GUI that only ever emits `isValid` values can never write
  /// a quality the daemon would have to clamp.
  public static func isValid(_ value: Int) -> Bool {
    range.contains(value)
  }

  /// Clamps `value` into `0...100` exactly the way the daemon does, so an
  /// out-of-range config value round-trips to the same value the daemon
  /// would have used.
  public static func clamp(_ value: Int) -> Int {
    min(range.upperBound, max(range.lowerBound, value))
  }

  /// Parses a `<dest>.jpeg_quality` config value into a clamped `0...100`
  /// quality, returning `nil` for a value the daemon wouldn't parse as an
  /// integer -- a non-integer (blank, non-numeric) or a value outside the
  /// 32-bit range -- so the caller can fall back to the default. The daemon's
  /// `ParseJpegQuality` uses a 32-bit `std::stoi`, so a value above
  /// `Int32.max` throws `out_of_range` and leaves the field at its default;
  /// rejecting it here first (before clamping) keeps the GUI in step rather
  /// than reading e.g. `3000000000` as a clamped 100. Same guard `Dpi`/
  /// `PositiveInt` use (see #120).
  public static func parse(_ string: String) -> Int? {
    guard let value = Int(string), value >= Int(Int32.min), value <= Int(Int32.max) else {
      return nil
    }
    return clamp(value)
  }
}
