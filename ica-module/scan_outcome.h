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

}  // namespace ica
}  // namespace brscan

#endif  // BRSCAN_ICA_SCAN_OUTCOME_H_
