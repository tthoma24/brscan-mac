import Foundation

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#endif

/// The file-access seam `ConfigStore` (task 1e.9) uses for everything that
/// touches `~/.config/brscan-scand.conf`: an existence check, a read, and
/// the two primitives an atomic save composes from -- write a temp file,
/// then rename it into place. Injected into `ConfigStore` so its load/save/
/// first-run logic, including the temp-then-rename sequencing, can be
/// exercised against an in-memory fake in tests and never touch the real
/// filesystem; `LocalConfigFileSystem` below is the app's real
/// implementation.
public protocol ConfigFileSystem {
  /// Whether a file exists at `url`.
  func fileExists(at url: URL) -> Bool

  /// Reads `url`'s contents as UTF-8 text.
  func contents(of url: URL) throws -> String

  /// Writes `text` to `url` as UTF-8, creating or overwriting `url`
  /// directly. Not itself atomic -- `ConfigStore` uses this only to write a
  /// temp file, then calls `moveItem(at:to:)` to install it.
  func write(_ text: String, to url: URL) throws

  /// Moves (renames) `sourceURL` to `destinationURL`, replacing any
  /// existing file at the destination. The "install" half of a
  /// temp-then-rename atomic save.
  func moveItem(at sourceURL: URL, to destinationURL: URL) throws
}

/// Errors a `ConfigFileSystem` implementation throws.
public enum ConfigStoreError: Error, Equatable {
  /// `moveItem(at:to:)`'s underlying rename failed. `errno` is the C
  /// `errno` value from the failing `rename()` call.
  case atomicWriteFailed(errno: Int32)
}

/// The real `ConfigFileSystem`: reads and writes through `FileManager` and
/// installs via the POSIX `rename()` syscall, so the temp-then-rename step
/// is a true atomic replace on the same volume -- mirroring
/// `BrscanConfigCore.ConfigDocument.write(to:)`'s own approach (see that
/// type), but behind this injectable seam instead of baked into
/// `ConfigDocument` itself.
public struct LocalConfigFileSystem: ConfigFileSystem {
  public init() {}

  public func fileExists(at url: URL) -> Bool {
    FileManager.default.fileExists(atPath: url.path)
  }

  public func contents(of url: URL) throws -> String {
    try String(contentsOf: url, encoding: .utf8)
  }

  public func write(_ text: String, to url: URL) throws {
    let directory = url.deletingLastPathComponent()
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try text.write(to: url, atomically: false, encoding: .utf8)
  }

  public func moveItem(at sourceURL: URL, to destinationURL: URL) throws {
    if rename(sourceURL.path, destinationURL.path) != 0 {
      let renameErrno = errno
      try? FileManager.default.removeItem(at: sourceURL)
      throw ConfigStoreError.atomicWriteFailed(errno: renameErrno)
    }
  }
}
