import Foundation

@testable import BrscanConfigApp

/// An in-memory `ConfigFileSystem`, so `ConfigStoreTests` never touches the
/// real filesystem. Also records every `write(_:to:)`/`moveItem(at:to:)`
/// call in `operations`, in order, so a test can assert a save actually
/// went through the temp-then-rename sequence rather than writing the
/// target file directly.
final class FakeConfigFileSystem: ConfigFileSystem {
  /// One recorded call, in the order it happened.
  enum Operation: Equatable {
    case write(path: String)
    case move(from: String, to: String)
  }

  private(set) var files: [String: String] = [:]
  private(set) var operations: [Operation] = []

  /// Seeds a file at `path` (relative to nothing in particular -- callers
  /// pass whatever `URL.path` they'll later look up), as if it already
  /// existed on disk before the test started.
  func seedFile(at url: URL, contents: String) {
    files[url.path] = contents
  }

  func fileExists(at url: URL) -> Bool {
    files[url.path] != nil
  }

  func contents(of url: URL) throws -> String {
    guard let text = files[url.path] else {
      throw CocoaError(.fileReadNoSuchFile)
    }
    return text
  }

  func write(_ text: String, to url: URL) throws {
    operations.append(.write(path: url.path))
    files[url.path] = text
  }

  func moveItem(at sourceURL: URL, to destinationURL: URL) throws {
    operations.append(.move(from: sourceURL.path, to: destinationURL.path))
    guard let text = files[sourceURL.path] else {
      throw CocoaError(.fileNoSuchFile)
    }
    files[destinationURL.path] = text
    files.removeValue(forKey: sourceURL.path)
  }
}
