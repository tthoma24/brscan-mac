import Foundation

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#endif

/// An error `ConfigDocument.write(to:)` throws when it can't atomically
/// install the new file contents.
public enum ConfigDocumentError: Error, Equatable {
  /// The temp-file-then-rename step failed. `errno` is the C `errno` value
  /// from the failing `rename()` call.
  case atomicWriteFailed(errno: Int32)
}

/// Reads and writes `~/.config/brscan-scand.conf`-style files while
/// preserving everything the caller doesn't touch: comments, blank lines,
/// key order, and unrecognized keys. Mirrors the tolerant `KEY = VALUE`
/// parser in `daemon/config.cpp`'s `ParseConfig()` (see that file) so the
/// GUI writes exactly what the daemon reads.
///
/// This type is format-only and key-agnostic: it has no notion of which
/// keys are valid or what their values should be. That vocabulary is a
/// separate, later task (see docs/PLAN-1E-DESIGN.md's anti-drift section).
public struct ConfigDocument: Equatable {

  /// One line of the file, classified the same way `ParseConfig()`
  /// classifies it.
  enum Kind: Equatable {
    /// An empty line, or one containing only whitespace.
    case blank
    /// A line whose first non-whitespace character is `#`.
    case comment
    /// A recognized `key = value` line. `key` and `value` are trimmed, the
    /// same way `daemon/config.cpp`'s `Trim()` trims them.
    case entry(key: String, value: String)
    /// Anything else: no `=` on the line, or an empty key before the `=`.
    /// Passed through unchanged, the same way the daemon's parser silently
    /// ignores it.
    case other
  }

  struct Line: Equatable {
    var raw: String
    var kind: Kind
  }

  private var lines: [Line]

  /// Whether the source text ended with a trailing newline. Preserved so a
  /// round-trip of an unmodified file reproduces it exactly.
  private var hasTrailingNewline: Bool

  // MARK: Trimming, mirroring daemon/config.cpp's Trim()

  /// The exact set of characters `daemon/config.cpp`'s `Trim()` strips from
  /// both ends of a line, a key, or a value: space, tab, CR, LF.
  private static let trimCharacters: Set<Character> = [" ", "\t", "\r", "\n"]

  private static func trim(_ s: Substring) -> String {
    var start = s.startIndex
    var end = s.endIndex
    while start < end, trimCharacters.contains(s[start]) {
      start = s.index(after: start)
    }
    while end > start, trimCharacters.contains(s[s.index(before: end)]) {
      end = s.index(before: end)
    }
    return String(s[start..<end])
  }

  // MARK: Parsing

  private static func classify(_ raw: String) -> Line {
    let trimmed = trim(Substring(raw))
    if trimmed.isEmpty {
      return Line(raw: raw, kind: .blank)
    }
    if trimmed.first == "#" {
      return Line(raw: raw, kind: .comment)
    }
    guard let eqIndex = trimmed.firstIndex(of: "=") else {
      return Line(raw: raw, kind: .other)
    }
    let key = trim(trimmed[trimmed.startIndex..<eqIndex])
    let value = trim(trimmed[trimmed.index(after: eqIndex)...])
    if key.isEmpty {
      return Line(raw: raw, kind: .other)
    }
    return Line(raw: raw, kind: .entry(key: key, value: value))
  }

  /// Splits `text` into raw lines on `"\n"`, reporting separately whether
  /// the text ended with a trailing newline -- so serialization can
  /// reproduce the file's original ending exactly.
  private static func splitLines(_ text: String) -> (lines: [String], hasTrailingNewline: Bool) {
    if text.isEmpty {
      return ([], false)
    }
    let trailing = text.hasSuffix("\n")
    var parts = text.components(separatedBy: "\n")
    if trailing {
      parts.removeLast()
    }
    return (parts, trailing)
  }

  // MARK: Creating a document

  /// An empty document (no lines), as a starting point for a config file
  /// created from scratch. Serializes to `""`.
  public init() {
    self.lines = []
    self.hasTrailingNewline = false
  }

  /// Parses `text` as a whole config file's contents into an ordered list
  /// of lines. Parsing never fails -- an unparsable line is kept, unchanged,
  /// as `.other`, the same way the daemon's own parser silently ignores a
  /// line it can't make sense of.
  public init(text: String) {
    let (rawLines, trailing) = ConfigDocument.splitLines(text)
    self.lines = rawLines.map { ConfigDocument.classify($0) }
    self.hasTrailingNewline = trailing
  }

  /// Reads and parses `url`'s contents as UTF-8 text.
  public static func load(from url: URL) throws -> ConfigDocument {
    let text = try String(contentsOf: url, encoding: .utf8)
    return ConfigDocument(text: text)
  }

  // MARK: Reading

  /// The value of the last `key = value` line for `key`, or `nil` if `key`
  /// has no active (non-comment) line. Mirrors `ParseConfig()`'s
  /// last-one-wins behavior when a key appears more than once.
  public func value(for key: String) -> String? {
    var result: String?
    for line in lines {
      if case let .entry(k, v) = line.kind, k == key {
        result = v
      }
    }
    return result
  }

  // MARK: Writing

  /// Sets `key`'s value to `value`. If `key` already has an active line,
  /// that line is replaced in place (same position in the file); if more
  /// than one line sets `key`, the last one -- the one that actually wins,
  /// per `value(for:)` -- is the one replaced, and any earlier, already-
  /// shadowed lines for `key` are left untouched. If `key` has no active
  /// line, a new `key = value` line is appended at the end of the file --
  /// a commented-out example for `key` is never uncommented, so the
  /// example stays intact and the diff stays obvious.
  ///
  /// Every other line -- comments, blank lines, other keys -- is left
  /// byte-for-byte unchanged.
  ///
  /// `value` is written as a single physical line: an embedded `"\n"` or
  /// `"\r"` (e.g. from pasted multi-line text into a bound `TextField`)
  /// would otherwise split one logical `key = value` entry into two lines
  /// on disk, corrupting the file -- the continuation would parse back as
  /// its own unrelated `.other` line, and `daemon/config.cpp`'s own
  /// line-based parser would silently misread it the same way. So any line
  /// break in `value` is collapsed to a space before writing (Review
  /// finding M1).
  public mutating func setValue(_ value: String, for key: String) {
    let singleLineValue = ConfigDocument.collapseLineBreaks(value)
    let newLine = Line(raw: "\(key) = \(singleLineValue)", kind: .entry(key: key, value: singleLineValue))
    if let index = lastIndex(ofKey: key) {
      lines[index] = newLine
    } else {
      lines.append(newLine)
    }
  }

  /// Replaces every `"\r\n"`, `"\n"`, and `"\r"` in `value` with a single
  /// space, so it can never split a `key = value` line in two. See
  /// `setValue`'s doc comment.
  private static func collapseLineBreaks(_ value: String) -> String {
    value.replacingOccurrences(of: "\r\n", with: " ")
      .replacingOccurrences(of: "\n", with: " ")
      .replacingOccurrences(of: "\r", with: " ")
  }

  /// Removes every active `key = value` line for `key`, leaving the key
  /// entirely absent from the document -- unlike `setValue`, which always
  /// leaves an active line behind (even for an empty string). Use this when
  /// a field's "unset" state must let a downstream reader fall back to its
  /// own default rather than being pinned to an explicit empty value (see
  /// `DaemonConfig.General.apply`'s handling of an empty `display_name`). A
  /// no-op if `key` has no active line. Every other line -- comments, blank
  /// lines, other keys, and any already-shadowed duplicate line for `key`
  /// that a hand-edited file might contain -- is left byte-for-byte
  /// unchanged.
  public mutating func removeValue(for key: String) {
    lines.removeAll { line in
      if case .entry(let lineKey, _) = line.kind { return lineKey == key }
      return false
    }
  }

  private func lastIndex(ofKey key: String) -> Int? {
    var found: Int?
    for (index, line) in lines.enumerated() {
      if case let .entry(k, _) = line.kind, k == key {
        found = index
      }
    }
    return found
  }

  /// Renders the document back to text. Round-tripping an unmodified
  /// document (`ConfigDocument(text: original).serialized() == original`)
  /// reproduces the input byte-for-byte, including blank lines, comments,
  /// key order, unknown keys, and the trailing-newline style.
  public func serialized() -> String {
    let body = lines.map { $0.raw }.joined(separator: "\n")
    guard hasTrailingNewline else { return body }
    return body + "\n"
  }

  /// Writes `serialized()` to `url` atomically: the new contents are
  /// written to a temp file in the same directory, then renamed into
  /// place, so a crash or a concurrent reader never sees a truncated file.
  ///
  /// Two accepted side effects of standard temp-then-rename, worth calling
  /// out rather than treating as bugs (Review finding M4): if `url` is a
  /// symlink, the `rename()` replaces the link itself with a regular file
  /// (the link doesn't survive); and the temp file is created with default
  /// permissions (subject to the process's umask), so a config file that
  /// was, say, `0600` comes back `0644` after a save. Neither matters for
  /// this app's data (a config file with no secrets, always written to a
  /// plain path under `~/.config`), so this is a deliberate choice, not an
  /// oversight.
  public func write(to url: URL) throws {
    let text = serialized()
    let directory = url.deletingLastPathComponent()
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)

    let tempURL = directory.appendingPathComponent(".\(url.lastPathComponent).tmp-\(UUID().uuidString)")
    try text.write(to: tempURL, atomically: false, encoding: .utf8)

    if rename(tempURL.path, url.path) != 0 {
      let renameErrno = errno
      try? FileManager.default.removeItem(at: tempURL)
      throw ConfigDocumentError.atomicWriteFailed(errno: renameErrno)
    }
  }
}
