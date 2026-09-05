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
enum PositiveInt {
  static func parse(_ string: String) -> Int? {
    guard let value = Int(string), value > 0 else { return nil }
    return value
  }
}
