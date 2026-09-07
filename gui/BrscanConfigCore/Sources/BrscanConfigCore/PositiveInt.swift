/// Parses a positive integer the way `daemon/config.cpp`'s
/// `ParsePositiveInt()` does: the whole string must be consumed as a
/// base-10 integer, and the result must be `> 0`. Shared by `Dpi` (the
/// `<dest>.dpi` value itself) and `Separation` (the `N` in `image:N` /
/// `page:N` / `every:N`).
///
/// This is slightly stricter than `std::stoi` about leading whitespace
/// (`Int.init(String)` doesn't skip it, `std::stoi` does), which only makes
/// this parser reject a handful of inputs the daemon would technically
/// accept (e.g. a stray space before the digits) -- not a case that occurs
/// in a config file the GUI itself writes.
///
/// The upper bound matters too: `std::stoi` parses into a 32-bit `int`, so
/// `ParsePositiveInt` throws `out_of_range` (and the daemon drops the key)
/// for a value above `INT_MAX`, whereas Swift's `Int.init(String)` parses a
/// 64-bit `Int`. So a value `> Int32.max` is rejected here too, keeping the
/// GUI's notion of "valid" in step with the daemon's 32-bit range.
enum PositiveInt {
  static func parse(_ string: String) -> Int? {
    guard let value = Int(string), value > 0, value <= Int(Int32.max) else { return nil }
    return value
  }
}
