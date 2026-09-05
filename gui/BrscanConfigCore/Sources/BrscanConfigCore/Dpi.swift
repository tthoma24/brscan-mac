/// `<dest>.dpi` isn't an enumerated set like `OptionValueSets`' members --
/// `daemon/config.cpp`'s `ParsePositiveInt()` (applied to both `x_dpi` and
/// `y_dpi`) accepts any positive integer -- so this validates the *shape*
/// of the value instead of membership in a token list. See
/// `GeneratedOptionSets.swift`'s `OptionSets.dpiDefault` for the documented
/// default (300).
public enum Dpi {
  /// Whether `value` is a dpi the daemon would accept: strictly positive.
  public static func isValid(_ value: Int) -> Bool {
    value > 0
  }

  /// Parses a `<dest>.dpi` config value the way the daemon does, returning
  /// `nil` for anything that isn't a positive integer (blank, non-numeric,
  /// zero, or negative).
  public static func parse(_ string: String) -> Int? {
    PositiveInt.parse(string)
  }
}
