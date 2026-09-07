#include "scan_output.h"

namespace brscan::cli {

int ExitCodeFor(brscan::Status status) {
  switch (status) {
    case brscan::Status::kBusy: return 10;
    case brscan::Status::kNoPaper: return 11;
    case brscan::Status::kCancelled: return 12;
    case brscan::Status::kTimeout: return 13;
    case brscan::Status::kIoError: return 14;
    case brscan::Status::kProtocolError: return 15;
    case brscan::Status::kOk: return 0;
  }
  return 1;
}

}  // namespace brscan::cli
