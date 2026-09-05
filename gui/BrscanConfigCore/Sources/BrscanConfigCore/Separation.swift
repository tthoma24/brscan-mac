/// The parsed form of a `<dest>.separation` config value. Mirrors
/// `daemon/config.cpp`'s `ParseSeparationString()`: `combine`/`off` both
/// collapse to a single "don't split" case, `image:N` splits output into
/// one file per N images, and `page:N` splits into one file per N pages.
///
/// `OptionSets.separationMode` (Task 1e.1's generated constants) lists the
/// four vendor-facing mode names (`combine`, `off`, `image`, `page`) this
/// type's cases are built from -- see that array's doc comment for why
/// `every` (a backward-compat alias, not a fifth mode) isn't among them.
public enum Separation: Equatable {
  /// `combine` or `off`: don't split output into multiple files.
  case combine
  /// `image:N`: one output file per N images.
  case image(Int)
  /// `page:N`: one output file per N pages.
  case page(Int)
}

/// Parsing and serializing `Separation` values to/from the daemon's
/// `<dest>.separation` config spelling.
public enum SeparationCodec {
  private static let imagePrefix = "image:"
  private static let pagePrefix = "page:"
  /// A pre-existing backward-compat alias for `image:N` (same mode,
  /// different spelling) -- see `daemon/config.cpp`'s
  /// `ParseSeparationString` and `config/option-sets.json`'s
  /// `separation_mode._comment`. Deliberately not one of
  /// `OptionSets.separationMode`'s four tokens: it parses in, but is never
  /// re-emitted by `serialize(_:)`.
  private static let everyPrefix = "every:"

  /// Parses a `<dest>.separation` config value, returning `nil` for
  /// anything the daemon's own parser would also reject (an unrecognized
  /// mode name, or a missing/non-positive/non-numeric `N`).
  public static func parse(_ string: String) -> Separation? {
    // "combine" and "off" are the two suffix-less modes in
    // OptionSets.separationMode; both mean "don't split".
    if string == "combine" || string == "off" {
      return .combine
    }
    if string.hasPrefix(imagePrefix), let n = PositiveInt.parse(String(string.dropFirst(imagePrefix.count))) {
      return .image(n)
    }
    if string.hasPrefix(pagePrefix), let n = PositiveInt.parse(String(string.dropFirst(pagePrefix.count))) {
      return .page(n)
    }
    if string.hasPrefix(everyPrefix), let n = PositiveInt.parse(String(string.dropFirst(everyPrefix.count))) {
      return .image(n)
    }
    return nil
  }

  /// Serializes a `Separation` back to the daemon's config spelling.
  /// Round-trips `image`/`page` exactly (`.image(4)` -> `"image:4"`); a
  /// value parsed from `every:N` re-serializes as `image:N`, never `every`,
  /// since `every` is a parse-only alias for `image`. `.combine` always
  /// serializes as `"combine"`, even if the original text on disk was
  /// `"off"` -- both mean the same thing to the daemon, so there's nothing
  /// left to preserve.
  public static func serialize(_ separation: Separation) -> String {
    switch separation {
    case .combine:
      return "combine"
    case .image(let n):
      return "\(imagePrefix)\(n)"
    case .page(let n):
      return "\(pagePrefix)\(n)"
    }
  }
}
