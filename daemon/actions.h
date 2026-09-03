#pragma once

#include <string>

#include "brscan/types.h"
#include "config.h"

// The FUNC dispatch point: what happens after a button-triggered scan has
// already been pulled and written to disk. See reference/plan-master.md's
// Plan 1b Task 5 for the full per-destination behavior this stands in for
// today.
namespace brscan::scand {

// Performs the destination action for `func` (FILE/IMAGE/OCR/EMAIL) on the
// scan already saved at `saved_path` (see HandleButtonEvent in
// daemon/handle_event.h, which calls this after WriteOutput succeeds).
//
//   - FILE: a no-op. Saving the file *is* the FILE action -- by the time
//     this runs, WriteOutput has already written it to `saved_path`.
//   - IMAGE, OCR, EMAIL: not yet implemented (Task 5). Logs a message
//     naming the FUNC and `saved_path`, and returns Status::kOk so a
//     button press against an unfinished action still leaves the caller
//     with a usable saved file instead of reporting failure.
//   - Any other string: treated the same as the not-yet-implemented cases
//     (logged, kOk) rather than as an error, since the scan itself already
//     succeeded and was saved.
//
// `cfg` is accepted (and currently unused) so Task 5's per-destination
// settings (e.g. an EMAIL recipient, an IMAGE target app) don't require
// changing this signature.
Status PerformAction(const std::string& func, const std::string& saved_path,
                      const Config& cfg);

}  // namespace brscan::scand
