// Tests for the finished-scan classifier (ica-module/scan_outcome.h).
//
// A pure, hermetic unit: no ICADevices, no framework, no device. It maps a
// libbrscan RunScan Status (plus the scan source and whether any page came back)
// to the outcome the ICA module reports -- in particular the ADF-feeder-empty
// case (Plan 2 Task 23), which replaces a generic device-internal error when an
// ADF scan produces no page with kNoPaper or kTimeout.

#include "scan_outcome.h"

#include <gtest/gtest.h>

#include "brscan/types.h"

namespace brscan::ica {
namespace {

// ---------------------------------------------------------------------
// ADF feeder-empty: no page + kNoPaper or kTimeout, feeder selected.
// ---------------------------------------------------------------------

TEST(ClassifyScanOutcomeTest, AdfNoPagesNoPaperIsFeederEmpty) {
  EXPECT_EQ(ClassifyScanOutcome(Source::kAdf, /*produced_pages=*/false,
                                Status::kNoPaper),
            ScanOutcome::kAdfFeederEmpty);
}

TEST(ClassifyScanOutcomeTest, AdfNoPagesTimeoutIsFeederEmpty) {
  EXPECT_EQ(ClassifyScanOutcome(Source::kAdf, /*produced_pages=*/false,
                                Status::kTimeout),
            ScanOutcome::kAdfFeederEmpty);
}

// A page came off the feeder before it emptied: NOT feeder-empty. kNoPaper /
// kTimeout after real pages is a generic failure, not "the feeder was empty".
TEST(ClassifyScanOutcomeTest, AdfWithPagesIsNotFeederEmpty) {
  EXPECT_EQ(ClassifyScanOutcome(Source::kAdf, /*produced_pages=*/true,
                                Status::kNoPaper),
            ScanOutcome::kFailed);
  EXPECT_EQ(ClassifyScanOutcome(Source::kAdf, /*produced_pages=*/true,
                                Status::kTimeout),
            ScanOutcome::kFailed);
}

// ---------------------------------------------------------------------
// ADF paper jam (C16): kPaperJam maps straight through, unambiguously.
// ---------------------------------------------------------------------

// A jammed ADF feed (a lone 0xc3 at ESC X -> libbrscan kPaperJam) classifies as
// the paper-jam outcome, distinct from the empty feeder above and from a generic
// failure. The feeder is the source and no page came back.
TEST(ClassifyScanOutcomeTest, AdfNoPagesPaperJamIsPaperJam) {
  EXPECT_EQ(ClassifyScanOutcome(Source::kAdf, /*produced_pages=*/false,
                                Status::kPaperJam),
            ScanOutcome::kPaperJam);
}

// ---------------------------------------------------------------------
// Flatbed is unaffected: it never maps to the feeder-empty outcome.
// ---------------------------------------------------------------------

TEST(ClassifyScanOutcomeTest, FlatbedNoPaperIsFailure) {
  EXPECT_EQ(ClassifyScanOutcome(Source::kFlatbed, /*produced_pages=*/false,
                                Status::kNoPaper),
            ScanOutcome::kFailed);
}

TEST(ClassifyScanOutcomeTest, FlatbedTimeoutIsFailure) {
  EXPECT_EQ(ClassifyScanOutcome(Source::kFlatbed, /*produced_pages=*/false,
                                Status::kTimeout),
            ScanOutcome::kFailed);
}

// ---------------------------------------------------------------------
// Ok and cancel pass through for both sources, regardless of page count.
// ---------------------------------------------------------------------

TEST(ClassifyScanOutcomeTest, OkIsOk) {
  EXPECT_EQ(ClassifyScanOutcome(Source::kAdf, /*produced_pages=*/true,
                                Status::kOk),
            ScanOutcome::kOk);
  EXPECT_EQ(ClassifyScanOutcome(Source::kFlatbed, /*produced_pages=*/false,
                                Status::kOk),
            ScanOutcome::kOk);
}

TEST(ClassifyScanOutcomeTest, CancelledIsCanceled) {
  EXPECT_EQ(ClassifyScanOutcome(Source::kAdf, /*produced_pages=*/false,
                                Status::kCancelled),
            ScanOutcome::kCanceled);
  EXPECT_EQ(ClassifyScanOutcome(Source::kFlatbed, /*produced_pages=*/true,
                                Status::kCancelled),
            ScanOutcome::kCanceled);
}

// C15: a device Stop-button cancel surfaces from libbrscan as Status::kCancelled
// (a lone 0x86 at ESC X, feeder selected, no page delivered; see scanner.cpp and
// PROVENANCE.md). It must classify as the SAME clean canceled outcome a host
// cancel produces AND yield no error-string key (nullptr) -- so no jam, empty, or
// generic dialog appears for a Stop-button cancel; the scan ends cleanly.
//
// The module (RunScanSynchronous) additionally posts a canonical
// kICANotificationTypeTransactionCanceled -- the distinct user-cancel signal
// Apple's VirtualScanner sample sends -- before the final ScannerScanDone(noErr),
// for both this device cancel and a host Cancel. That notification is NOT a
// DeviceStatusError, so it does not change the "no error-string key" fact asserted
// here. It is posted from module_main.mm, which is not compiled into this unit
// suite (SendScannerNotification wraps the real ICDSendNotification), so the post
// itself is confirmed device-in-the-loop (docs/RUNBOOK-plan-2-ica.md row C15), not
// here; this test still pins the clean-outcome/no-dialog classification it rests on.
TEST(ClassifyScanOutcomeTest, AdfDeviceStopCancelIsCleanCanceledNoDialog) {
  const ScanOutcome outcome = ClassifyScanOutcome(
      Source::kAdf, /*produced_pages=*/false, Status::kCancelled);
  EXPECT_EQ(outcome, ScanOutcome::kCanceled);
  EXPECT_EQ(ErrorStringKeyForOutcome(outcome, Status::kCancelled), nullptr);
}

// ---------------------------------------------------------------------
// Other transport/protocol errors are generic failures on either source.
// ---------------------------------------------------------------------

TEST(ClassifyScanOutcomeTest, IoAndProtocolErrorsAreFailure) {
  EXPECT_EQ(ClassifyScanOutcome(Source::kAdf, /*produced_pages=*/false,
                                Status::kIoError),
            ScanOutcome::kFailed);
  EXPECT_EQ(ClassifyScanOutcome(Source::kAdf, /*produced_pages=*/false,
                                Status::kProtocolError),
            ScanOutcome::kFailed);
  EXPECT_EQ(ClassifyScanOutcome(Source::kAdf, /*produced_pages=*/false,
                                Status::kBusy),
            ScanOutcome::kFailed);
}

// ---------------------------------------------------------------------
// ErrorStringKeyForOutcome: the Error.loctable KEY the module hands the host
// so Image Capture renders a readable scanner-error dialog (PR C). Pure and
// CoreFoundation-free -- module_main.mm wraps the returned key in a CFString.
// ---------------------------------------------------------------------

// Ok and cancel raise no error dialog: no key.
TEST(ErrorStringKeyForOutcomeTest, OkAndCanceledHaveNoKey) {
  EXPECT_EQ(ErrorStringKeyForOutcome(ScanOutcome::kOk, Status::kOk), nullptr);
  EXPECT_EQ(ErrorStringKeyForOutcome(ScanOutcome::kCanceled, Status::kCancelled),
            nullptr);
}

// Feeder-empty resolves to Image Capture's "Document feeder is empty."
TEST(ErrorStringKeyForOutcomeTest, FeederEmptyIsDFEmptyKey) {
  EXPECT_STREQ(
      ErrorStringKeyForOutcome(ScanOutcome::kAdfFeederEmpty, Status::kNoPaper),
      "kICAErrStrDFEmptyErr");
}

// Paper-jam resolves to Image Capture's "Document feeder has a paper jam or
// paper feed error." (C16) -- the jam-specific dialog, not the empty-feeder one.
TEST(ErrorStringKeyForOutcomeTest, PaperJamIsDFPaperKey) {
  EXPECT_STREQ(
      ErrorStringKeyForOutcome(ScanOutcome::kPaperJam, Status::kPaperJam),
      "kICAErrStrDFPaperErr");
}

// A protocol desync reads as a generic scan error ("An error occurred during
// scanning."), not a communication fault.
TEST(ErrorStringKeyForOutcomeTest, ProtocolErrorIsScanErr) {
  EXPECT_STREQ(
      ErrorStringKeyForOutcome(ScanOutcome::kFailed, Status::kProtocolError),
      "kICAErrStrScanErr");
}

// Transport faults (socket I/O, wait elapsed) read as a communication error.
TEST(ErrorStringKeyForOutcomeTest, IoAndTimeoutAreScannerComErr) {
  EXPECT_STREQ(
      ErrorStringKeyForOutcome(ScanOutcome::kFailed, Status::kIoError),
      "kICAErrStrScannerComErr");
  EXPECT_STREQ(
      ErrorStringKeyForOutcome(ScanOutcome::kFailed, Status::kTimeout),
      "kICAErrStrScannerComErr");
}

// Busy: the single scan connection is held elsewhere.
TEST(ErrorStringKeyForOutcomeTest, BusyIsScannerBusyErr) {
  EXPECT_STREQ(ErrorStringKeyForOutcome(ScanOutcome::kFailed, Status::kBusy),
               "kICAErrStrScannerBusyErr");
}

// Any other status behind kFailed still gets a readable generic scan error.
TEST(ErrorStringKeyForOutcomeTest, UnclassifiedFailureFallsBackToScanErr) {
  EXPECT_STREQ(ErrorStringKeyForOutcome(ScanOutcome::kFailed, Status::kNoPaper),
               "kICAErrStrScanErr");
}

}  // namespace
}  // namespace brscan::ica
