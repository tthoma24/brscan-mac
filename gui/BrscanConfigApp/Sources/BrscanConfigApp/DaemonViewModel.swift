import Combine
import Foundation

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#endif

/// View model (task 1e.10) for the `brscan-scand` `LaunchAgent`'s status and
/// the "Save & apply" action: whether the agent is installed/stopped/
/// running, and reloading it (`SIGHUP`, the handler task 1e.0 added) after a
/// config save. All `launchctl` access goes through the injected
/// `DaemonControlling` seam, so tests never shell out to the real
/// `launchctl` -- mirrors `ConfigStore`'s use of `ConfigFileSystem` (task
/// 1e.9).
public final class DaemonViewModel: ObservableObject {

  /// The `brscan-scand` `LaunchAgent`'s state, as classified from
  /// `launchctl print`'s output.
  public enum State: Equatable {
    /// No such agent is bootstrapped in this domain -- either it was never
    /// installed, or the lookup itself failed. Also the state before the
    /// first `refreshState()` call.
    case notInstalled
    /// The agent is bootstrapped but not currently running.
    case stopped
    /// The agent is bootstrapped and running -- the only state in which
    /// `saveAndApply(save:)` will send it a reload signal.
    case running

    /// Short status text for the window and `MenuBarExtra`. Google
    /// Developer Style Guide sentence case.
    public var statusText: String {
      switch self {
      case .notInstalled: return "Not installed"
      case .stopped: return "Stopped"
      case .running: return "Running"
      }
    }
  }

  /// The result of a `saveAndApply(save:)` call, exposed so the view can
  /// show guidance or an error alongside the plain Save flow.
  public enum ApplyOutcome: Equatable {
    /// The config saved and the running daemon was signaled to reload it.
    case appliedAndReloaded
    /// The config saved, but the daemon wasn't running to reload -- not an
    /// error, just guidance that the change takes effect next time it
    /// starts.
    case savedOnly
    /// The config saved, but the reload signal itself failed.
    case signalFailed
    /// The save itself failed; no reload was attempted.
    case saveFailed

    /// User-facing message for the Save bar. Google Developer Style Guide
    /// sentence case, plain language.
    public var message: String {
      switch self {
      case .appliedAndReloaded:
        return "Saved. The daemon reloaded your changes."
      case .savedOnly:
        return "Saved. The daemon isn't running -- your changes are saved and will apply "
          + "when it next starts. See Setup in docs/BUTTON.md."
      case .signalFailed:
        return "Saved, but the daemon didn't reload. Try Save & Apply again, or restart the daemon."
      case .saveFailed:
        return "Couldn't save your changes. Check file permissions and try again."
      }
    }
  }

  /// The `brscan-scand` `LaunchAgent`'s label, read from
  /// `config/com.brscan.scand.plist.example`'s `Label` key (also documented
  /// in `docs/BUTTON.md`) -- not derived at runtime, since it's fixed by
  /// that file, but never hardcoded anywhere else in this app.
  public static let agentLabel = "com.brscan.scand"

  @Published public private(set) var state: State = .notInstalled
  @Published public private(set) var lastApplyOutcome: ApplyOutcome?

  private let control: DaemonControlling
  private let label: String

  public init(control: DaemonControlling = LaunchctlDaemonControl(), label: String = DaemonViewModel.agentLabel) {
    self.control = control
    self.label = label
  }

  /// The `launchctl` domain target for this agent: `gui/<uid>/<label>`,
  /// with the uid derived at runtime via `getuid()` -- never hardcoded, so
  /// this works under whichever account is running the app.
  public var domainTarget: String {
    "gui/\(getuid())/\(label)"
  }

  /// Re-queries `launchctl` and updates `state`. A lookup failure (`nil`
  /// from `printAgent`) is classified as `.notInstalled`, per this seam's
  /// contract -- covers both "never installed" and any other lookup error.
  public func refreshState() {
    state = Self.classify(control.printAgent(domainTarget: domainTarget))
  }

  /// `launchctl print`'s output includes a `state = running` line while the
  /// agent is active, and something else (typically `state = not running`)
  /// otherwise. Matching on that line, rather than parsing the full
  /// property-list-like output, is enough to tell running from stopped.
  private static func classify(_ printOutput: String?) -> State {
    guard let printOutput else { return .notInstalled }
    return printOutput.contains("state = running") ? .running : .stopped
  }

  /// Runs `save` (typically `ConfigStore.save`), then applies the result:
  /// if the daemon is running, sends it the reload signal and reports
  /// whether that succeeded; otherwise the save still counts as complete,
  /// with `.savedOnly` guidance rather than an error. A failing `save`
  /// short-circuits before any reload attempt. `state` is refreshed
  /// immediately before deciding whether to signal, so this reflects the
  /// daemon's current state rather than whatever `refreshState()` last
  /// cached.
  @discardableResult
  public func saveAndApply(save: () throws -> Void) -> ApplyOutcome {
    do {
      try save()
    } catch {
      return recordOutcome(.saveFailed)
    }

    refreshState()
    guard state == .running else {
      return recordOutcome(.savedOnly)
    }

    return recordOutcome(control.sendHup(domainTarget: domainTarget) ? .appliedAndReloaded : .signalFailed)
  }

  /// Runs `save` (typically `ConfigStore.save`) directly, without touching
  /// the daemon -- the plain **Save** button's action, as opposed to
  /// `saveAndApply(save:)`, which also signals a running daemon to reload.
  /// A failure is recorded as `.saveFailed` through `lastApplyOutcome`, the
  /// same outcome (and the same user-facing message) `saveAndApply` reports
  /// for a failed save, so the two adjacent buttons behave consistently on
  /// an atomic-write failure (Review finding I3) -- previously the plain
  /// Save button discarded the error with `try?` and showed nothing. A
  /// successful save clears `lastApplyOutcome` (rather than setting any
  /// reload-specific guidance, since no reload was attempted), so a stale
  /// message from an earlier failed attempt doesn't linger after a
  /// subsequent save succeeds.
  @discardableResult
  public func saveOnly(save: () throws -> Void) -> Bool {
    do {
      try save()
    } catch {
      _ = recordOutcome(.saveFailed)
      return false
    }
    lastApplyOutcome = nil
    return true
  }

  private func recordOutcome(_ outcome: ApplyOutcome) -> ApplyOutcome {
    lastApplyOutcome = outcome
    return outcome
  }
}
