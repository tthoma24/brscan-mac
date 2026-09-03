// Shared output helpers for turning a completed brscan::ScanResult into a
// file on disk, and for describing/exit-coding a failed brscan::Status.
// Used by brscan-cli today; intended to be shared with the scan daemon too.
#pragma once

#include <string>

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

}  // namespace brscan::cli
