#include "actions.h"

#include <iostream>

namespace brscan::scand {

Status PerformAction(const std::string& func, const std::string& saved_path,
                      const Config& cfg) {
  (void)cfg;  // Unused until Task 5 adds per-destination settings.

  if (func == kFuncFile) {
    std::cout << "[actions] FILE: scan saved to " << saved_path << "\n";
    return Status::kOk;
  }

  if (func == kFuncImage || func == kFuncOcr || func == kFuncEmail) {
    std::cout << "[actions] " << func
               << ": action not yet implemented (Plan 1b Task 5); scan "
                  "saved to "
               << saved_path << "\n";
    return Status::kOk;
  }

  std::cout << "[actions] unrecognized FUNC '" << func
             << "'; treating as no-op, scan saved to " << saved_path << "\n";
  return Status::kOk;
}

}  // namespace brscan::scand
