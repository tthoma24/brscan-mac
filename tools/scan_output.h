// CLI-specific glue for a completed or failed scan: mapping a brscan::Status
// to a process exit code. The shared, front-end-agnostic output primitives
// (WriteOutput/WritePages/PagePath and DescribeFailure) live in the
// brscan-output library instead -- see output/page_writer.h.
#pragma once

#include "brscan/types.h"

namespace brscan::cli {

int ExitCodeFor(brscan::Status status);

}  // namespace brscan::cli
