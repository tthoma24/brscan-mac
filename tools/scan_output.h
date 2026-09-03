// Shared output helpers for turning a completed brscan::ScanResult into a
// file on disk, and for describing/exit-coding a failed brscan::Status.
// Used by brscan-cli today; intended to be shared with the scan daemon too.
#pragma once

#include <string>
#include <vector>

#include "brscan/scanner.h"
#include "brscan/types.h"

namespace brscan::cli {

// A human-readable line for each failure Status RunScan can report, per
// libbrscan/scanner.h's doc comment.
std::string DescribeFailure(brscan::Status status);

int ExitCodeFor(brscan::Status status);

// Writes `result` to `path`: the JPEG bytes as-is for color, a binary PGM
// (P5) for gray (GRAY64 raw or GRAY256/RLENGTH, both PixelFormat::kGray),
// or a binary PBM (P4) for the 1-bit modes (TEXT/ERRDIF,
// PixelFormat::kBitonal -- see the bit-packing convention documented on
// PixelFormat::kBitonal in types.h, which is exactly what P4 expects, so
// `result.data` is written out unchanged). Returns false (after printing
// an error) if the file can't be opened for writing.
bool WriteOutput(const brscan::ScanResult& result, const std::string& path);

// Writes every page in `pages` to disk, per WriteOutput's format rules for
// each page's PixelFormat. A single-page vector writes exactly `path`,
// unchanged. A multi-page vector writes one numbered file per page instead:
// `path` with `-<NNN>` (1-based, zero-padded to 3 digits) inserted before
// its extension -- e.g. `scan.jpg` becomes `scan-001.jpg`, `scan-002.jpg`,
// and so on. Returns false (after WriteOutput has printed an error for the
// page that failed) if any page fails to write; pages before the failure
// are still left on disk.
bool WritePages(const std::vector<brscan::ScanResult>& pages,
                 const std::string& path);

}  // namespace brscan::cli
