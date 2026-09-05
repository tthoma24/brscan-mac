import XCTest

@testable import BrscanConfigApp

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#endif

/// Unit tests for `DaemonViewModel` (task 1e.10): state classification from
/// `launchctl print` output, "Save & apply" signaling only while running,
/// a signal failure surfacing as an error state after the save already
/// happened, and the domain target being well-formed. Every test drives a
/// `FakeDaemonControl`, never the real `launchctl`.
final class DaemonViewModelTests: XCTestCase {

  // MARK: State classification

  func testNotInstalledWhenLookupFails() {
    let control = FakeDaemonControl()
    control.printAgentResult = nil
    let viewModel = DaemonViewModel(control: control)

    viewModel.refreshState()

    XCTAssertEqual(viewModel.state, .notInstalled)
  }

  func testStoppedWhenAgentIsInstalledButNotRunning() {
    let control = FakeDaemonControl()
    control.printAgentResult = "\tstate = not running\n"
    let viewModel = DaemonViewModel(control: control)

    viewModel.refreshState()

    XCTAssertEqual(viewModel.state, .stopped)
  }

  func testRunningWhenAgentStateLineSaysRunning() {
    let control = FakeDaemonControl()
    control.printAgentResult = "\tstate = running\n"
    let viewModel = DaemonViewModel(control: control)

    viewModel.refreshState()

    XCTAssertEqual(viewModel.state, .running)
  }

  // MARK: Save & apply -- signals only when running

  func testSaveAndApplySendsHupOnlyWhenRunning() {
    let control = FakeDaemonControl()
    control.printAgentResult = "\tstate = running\n"
    let viewModel = DaemonViewModel(control: control)
    var saveCalled = false

    let outcome = viewModel.saveAndApply { saveCalled = true }

    XCTAssertTrue(saveCalled, "save must happen regardless of daemon state")
    XCTAssertEqual(outcome, .appliedAndReloaded)
    XCTAssertEqual(viewModel.lastApplyOutcome, .appliedAndReloaded)
    XCTAssertEqual(control.sendHupCalls.count, 1)
  }

  func testSaveAndApplyDoesNotSignalWhenStopped() {
    let control = FakeDaemonControl()
    control.printAgentResult = "\tstate = not running\n"
    let viewModel = DaemonViewModel(control: control)
    var saveCalled = false

    let outcome = viewModel.saveAndApply { saveCalled = true }

    XCTAssertTrue(saveCalled, "save must still happen when the daemon is stopped")
    XCTAssertEqual(outcome, .savedOnly)
    XCTAssertTrue(control.sendHupCalls.isEmpty, "must not signal a stopped daemon")
  }

  func testSaveAndApplyDoesNotSignalWhenNotInstalled() {
    let control = FakeDaemonControl()
    control.printAgentResult = nil
    let viewModel = DaemonViewModel(control: control)
    var saveCalled = false

    let outcome = viewModel.saveAndApply { saveCalled = true }

    XCTAssertTrue(saveCalled, "save must still happen when the daemon isn't installed")
    XCTAssertEqual(outcome, .savedOnly)
    XCTAssertTrue(control.sendHupCalls.isEmpty, "must not signal an uninstalled daemon")
  }

  // MARK: Signal failure

  func testSaveAndApplySurfacesSignalFailureAfterSaveAlreadySucceeded() {
    let control = FakeDaemonControl()
    control.printAgentResult = "\tstate = running\n"
    control.sendHupResult = false
    let viewModel = DaemonViewModel(control: control)
    var saveCalled = false

    let outcome = viewModel.saveAndApply { saveCalled = true }

    XCTAssertTrue(saveCalled, "the save already happened before the signal was attempted")
    XCTAssertEqual(outcome, .signalFailed)
    XCTAssertEqual(control.sendHupCalls.count, 1, "a signal was attempted since the daemon was running")
  }

  func testSaveAndApplySurfacesSaveFailureWithoutAttemptingASignal() {
    let control = FakeDaemonControl()
    control.printAgentResult = "\tstate = running\n"
    let viewModel = DaemonViewModel(control: control)

    enum TestError: Error { case boom }
    let outcome = viewModel.saveAndApply { throw TestError.boom }

    XCTAssertEqual(outcome, .saveFailed)
    XCTAssertTrue(control.sendHupCalls.isEmpty, "must not signal when the save itself failed")
    XCTAssertTrue(control.printAgentCalls.isEmpty, "must not even check daemon state when the save failed")
  }

  // MARK: Plain Save -- surfaces a save failure, never touches the daemon (Review I3)

  func testSaveOnlySucceedsAndDoesNotConsultTheDaemonAtAll() {
    let control = FakeDaemonControl()
    let viewModel = DaemonViewModel(control: control)
    var saveCalled = false

    let succeeded = viewModel.saveOnly { saveCalled = true }

    XCTAssertTrue(saveCalled)
    XCTAssertTrue(succeeded)
    XCTAssertNil(viewModel.lastApplyOutcome)
    XCTAssertTrue(control.printAgentCalls.isEmpty, "plain Save must never check daemon state")
    XCTAssertTrue(control.sendHupCalls.isEmpty, "plain Save must never signal the daemon")
  }

  func testSaveOnlySurfacesAFailureAsSaveFailedInsteadOfSwallowingIt() {
    let control = FakeDaemonControl()
    let viewModel = DaemonViewModel(control: control)

    enum TestError: Error { case boom }
    let succeeded = viewModel.saveOnly { throw TestError.boom }

    XCTAssertFalse(succeeded)
    XCTAssertEqual(viewModel.lastApplyOutcome, .saveFailed)
    XCTAssertTrue(control.sendHupCalls.isEmpty)
  }

  func testSaveOnlyClearsAStalePreviousOutcomeOnSuccess() {
    let control = FakeDaemonControl()
    control.printAgentResult = "\tstate = running\n"
    control.sendHupResult = false
    let viewModel = DaemonViewModel(control: control)

    // First, an earlier Save & Apply leaves a stale failure message.
    _ = viewModel.saveAndApply {}
    XCTAssertEqual(viewModel.lastApplyOutcome, .signalFailed)

    // A subsequent plain Save that succeeds must not leave that stale
    // message showing.
    let succeeded = viewModel.saveOnly {}
    XCTAssertTrue(succeeded)
    XCTAssertNil(viewModel.lastApplyOutcome)
  }

  // MARK: Domain target composition

  func testDomainTargetIsComposedFromRuntimeUIDAndTheRealLabel() {
    let control = FakeDaemonControl()
    control.printAgentResult = "\tstate = running\n"
    let viewModel = DaemonViewModel(control: control)
    let expectedTarget = "gui/\(getuid())/\(DaemonViewModel.agentLabel)"

    viewModel.refreshState()
    XCTAssertEqual(control.printAgentCalls.last, expectedTarget)

    _ = viewModel.saveAndApply {}
    XCTAssertEqual(control.sendHupCalls.last, expectedTarget)
  }

  func testAgentLabelMatchesTheLaunchAgentPlist() {
    // config/com.brscan.scand.plist.example's <key>Label</key> value, also
    // documented in docs/BUTTON.md -- kept as one named constant rather than
    // a string literal scattered across call sites.
    XCTAssertEqual(DaemonViewModel.agentLabel, "com.brscan.scand")
  }
}
