import Foundation

/// The daemon-control seam `DaemonViewModel` (task 1e.10) uses for everything
/// that talks to the `brscan-scand` `LaunchAgent`: a state lookup and a
/// reload signal. Injected into `DaemonViewModel` so its classification and
/// "Save & apply" logic can be exercised against an in-memory fake in tests
/// and never shell out to the real `launchctl`; `LaunchctlDaemonControl`
/// below is the app's real implementation. Mirrors the `ConfigFileSystem`
/// seam task 1e.9 added for file access.
///
/// Both methods take the fully composed domain target (`gui/<uid>/<label>`)
/// rather than a bare label, so the uid-lookup-at-runtime behavior itself is
/// exercised by whatever calls this seam (`DaemonViewModel`), not hidden
/// inside it -- a test can assert the exact target a fake received.
public protocol DaemonControlling {
  /// Looks up the agent at `domainTarget` and returns its raw
  /// `launchctl print` output, or `nil` if the lookup failed -- no such
  /// agent bootstrapped in this domain, which `DaemonViewModel` classifies
  /// as "not installed" rather than treating as an error.
  func printAgent(domainTarget: String) -> String?

  /// Sends the agent at `domainTarget` a reload signal (`SIGHUP`, the
  /// handler task 1e.0 added to `brscan-scand`). Returns whether the
  /// underlying command exited successfully. Callers only invoke this when
  /// a prior `printAgent` classified the agent as running.
  func sendHup(domainTarget: String) -> Bool
}

/// The real `DaemonControlling`: shells out to `/bin/launchctl`.
///
/// Never hardcodes a uid or label -- `domainTarget` arrives pre-composed
/// from `DaemonViewModel`, which derives the uid at runtime (`getuid()`)
/// and pairs it with the agent's real label
/// (`config/com.brscan.scand.plist.example`'s `Label`, also documented in
/// `docs/BUTTON.md`).
public struct LaunchctlDaemonControl: DaemonControlling {
  public init() {}

  public func printAgent(domainTarget: String) -> String? {
    let result = Self.run(["print", domainTarget])
    return result.exitCode == 0 ? result.output : nil
  }

  public func sendHup(domainTarget: String) -> Bool {
    Self.run(["kill", "-s", "HUP", domainTarget]).exitCode == 0
  }

  /// Runs `/bin/launchctl <arguments>`, capturing combined stdout+stderr.
  /// A failure to even launch the process (e.g. `launchctl` missing) is
  /// reported as a non-zero exit code with empty output, which
  /// `printAgent`/`sendHup` both treat the same as a real command failure.
  private static func run(_ arguments: [String]) -> (exitCode: Int32, output: String) {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: "/bin/launchctl")
    process.arguments = arguments

    let outputPipe = Pipe()
    process.standardOutput = outputPipe
    process.standardError = outputPipe

    do {
      try process.run()
    } catch {
      return (-1, "")
    }

    let data = outputPipe.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    return (process.terminationStatus, String(data: data, encoding: .utf8) ?? "")
  }
}
