// Pure classification of a completed scan's libbrscan Status into the outcome
// the ICA module reports to the host. Split out of module_main.mm's
// RunScanSynchronous so the ADF-feeder-empty decision is unit-testable without
// the ICADevices framework or a live device (Plan 2 Task 23).
//
// Clean-room: written against libbrscan's public brscan::Status / brscan::Source
// (libbrscan/include/brscan/types.h) only. The mapping from these outcomes to
// concrete ICA notifications / ICAError codes stays in module_main.mm.

#ifndef BRSCAN_ICA_SCAN_OUTCOME_H_
#define BRSCAN_ICA_SCAN_OUTCOME_H_

#include "brscan/types.h"

namespace brscan {
namespace ica {

// What the module does with a finished RunScan.
enum class ScanOutcome {
  kOk,              // Deliver/finish the scan normally.
  kCanceled,        // Clean host cancel (RunScan returned kCancelled).
  kAdfFeederEmpty,  // ADF selected but no page was fed -> "feeder empty".
  kFailed,          // Any other non-OK status -> generic device failure.
};

// Classifies a completed scan. `produced_pages` is whether RunScan handed back
// at least one page. The ADF-feeder-empty outcome is reported ONLY when the
// feeder was selected, NO page came back, and the status is kNoPaper (nothing to
// feed) or kTimeout (the wait elapsed with nothing fed -- the current ~24 s
// empty-ADF hang, pending fast detection in libbrscan). Flatbed, and any scan
// that produced at least one page, are never classified as feeder-empty; every
// other non-OK status is a generic failure.
inline ScanOutcome ClassifyScanOutcome(Source source, bool produced_pages,
                                       Status status) {
  if (status == Status::kOk) return ScanOutcome::kOk;
  if (status == Status::kCancelled) return ScanOutcome::kCanceled;
  if (source == Source::kAdf && !produced_pages &&
      (status == Status::kNoPaper || status == Status::kTimeout)) {
    return ScanOutcome::kAdfFeederEmpty;
  }
  return ScanOutcome::kFailed;
}

// Maps a classified outcome to the Image Capture localized-string KEY that makes
// the host raise a readable scanner-error dialog, or nullptr when no dialog
// should be shown (PR C). The returned value is a plain C string -- a raw
// Error.loctable key, NOT a CoreFoundation object -- so this header stays
// framework-free and unit-testable like ClassifyScanOutcome; module_main.mm
// wraps the key in a CFString and posts it as the kICANotificationSubTypeKey of
// a kICANotificationTypeDeviceStatusError (see PostScannerError). The keys are
// interface facts: Image Capture resolves them via
// ICADevices.framework/.../Resources/Error.loctable (verified on this machine)
// to, respectively:
//   kICAErrStrDFEmptyErr      -> "Document feeder is empty."
//   kICAErrStrScanErr         -> "An error occurred during scanning."
//   kICAErrStrScannerComErr   -> "An error occurred while communicating with the
//                                 scanner."
//   kICAErrStrScannerBusyErr  -> "The scanner is busy."
// No Apple source was copied. Paper-jam is deliberately NOT mapped here: the
// key kICAErrStrDFPaperErr ("Document feeder has a paper jam or paper feed
// error.") exists, but the device's jam signature is uncaptured, and labeling a
// protocol desync as a jam would be wrong -- mapping it is a follow-up pending a
// captured jam signature.
inline const char* ErrorStringKeyForOutcome(ScanOutcome outcome,
                                            Status status) {
  switch (outcome) {
    case ScanOutcome::kOk:
    case ScanOutcome::kCanceled:
      // A successful scan or a clean host cancel is not an error: no dialog.
      return nullptr;
    case ScanOutcome::kAdfFeederEmpty:
      return "kICAErrStrDFEmptyErr";
    case ScanOutcome::kFailed:
      switch (status) {
        case Status::kProtocolError:
          // A wire-protocol desync is not a link fault; report the generic
          // scan error rather than implying a communication problem.
          return "kICAErrStrScanErr";
        case Status::kIoError:
        case Status::kTimeout:
          // A socket fault, or the wait elapsing, is a communication failure.
          return "kICAErrStrScannerComErr";
        case Status::kBusy:
          // The device allows a single scan connection; it is held elsewhere.
          return "kICAErrStrScannerBusyErr";
        default:
          // Any other status still gets a readable generic scan error.
          return "kICAErrStrScanErr";
      }
  }
  return "kICAErrStrScanErr";
}

}  // namespace ica
}  // namespace brscan

#endif  // BRSCAN_ICA_SCAN_OUTCOME_H_
