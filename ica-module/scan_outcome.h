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
  kPaperJam,        // ADF document-feeder paper jam / feed error (kPaperJam).
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
  // A document-feeder paper jam is unambiguous: libbrscan returns kPaperJam
  // only for an ADF scan whose feed jammed mid-page (a lone 0xc3 at ESC X),
  // never for the flatbed and never after real pages, so map it straight
  // through -- ahead of the ambiguous no-page kNoPaper/kTimeout cases below.
  if (status == Status::kPaperJam) return ScanOutcome::kPaperJam;
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
//   kICAErrStrDFPaperErr      -> "Document feeder has a paper jam or paper feed
//                                 error."
//   kICAErrStrScanErr         -> "An error occurred during scanning."
//   kICAErrStrScannerComErr   -> "An error occurred while communicating with the
//                                 scanner."
//   kICAErrStrScannerBusyErr  -> "The scanner is busy."
// No Apple source was copied. Paper-jam IS mapped (kICAErrStrDFPaperErr): unlike
// a generic protocol desync, the jam has a captured, unambiguous wire signature
// -- a lone 0xc3 status byte at ESC X (reference/c16-jam-imac.pcap; see
// PROVENANCE.md and scanner.cpp) -- which libbrscan reports as Status::kPaperJam
// and ClassifyScanOutcome maps to ScanOutcome::kPaperJam, so labeling it a jam
// is correct rather than a guess.
inline const char* ErrorStringKeyForOutcome(ScanOutcome outcome,
                                            Status status) {
  switch (outcome) {
    case ScanOutcome::kOk:
    case ScanOutcome::kCanceled:
      // A successful scan or a clean host cancel is not an error: no dialog.
      return nullptr;
    case ScanOutcome::kAdfFeederEmpty:
      return "kICAErrStrDFEmptyErr";
    case ScanOutcome::kPaperJam:
      return "kICAErrStrDFPaperErr";
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
