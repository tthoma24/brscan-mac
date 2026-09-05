import Foundation

@testable import BrscanConfigApp

/// A canned-response `DaemonControlling`, so `DaemonViewModelTests` never
/// shells out to the real `launchctl`. Records every call's `domainTarget`
/// so a test can assert the composed target is well-formed, and how many
/// times each method fired -- e.g. that `sendHup` was never called when the
/// agent isn't running.
final class FakeDaemonControl: DaemonControlling {
  /// Canned return value for `printAgent`. `nil` (the default) simulates a
  /// failed lookup -- "not installed".
  var printAgentResult: String?
  /// Canned return value for `sendHup`. Defaults to success.
  var sendHupResult = true

  private(set) var printAgentCalls: [String] = []
  private(set) var sendHupCalls: [String] = []

  func printAgent(domainTarget: String) -> String? {
    printAgentCalls.append(domainTarget)
    return printAgentResult
  }

  func sendHup(domainTarget: String) -> Bool {
    sendHupCalls.append(domainTarget)
    return sendHupResult
  }
}
