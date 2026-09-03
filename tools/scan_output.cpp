#include "scan_output.h"

#include <fstream>
#include <iostream>

namespace brscan::cli {

std::string DescribeFailure(brscan::Status status) {
  switch (status) {
    case brscan::Status::kBusy:
      return "scanner is busy (another job is in progress)";
    case brscan::Status::kNoPaper:
      return "no paper detected in the document feeder";
    case brscan::Status::kCancelled:
      return "scan was cancelled";
    case brscan::Status::kTimeout:
      return "scan timed out (the device stopped responding -- possibly "
             "cancelled at the panel)";
    case brscan::Status::kIoError:
      return "connection error talking to the scanner";
    case brscan::Status::kProtocolError:
      return "unexpected data from the scanner (protocol error)";
    case brscan::Status::kOk:
      return "ok";
  }
  return "unknown error";
}

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

bool WriteOutput(const brscan::ScanResult& result, const std::string& path) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    std::cerr << "Could not open '" << path << "' for writing\n";
    return false;
  }

  switch (result.format) {
    case brscan::PixelFormat::kRgb:
      f.write(reinterpret_cast<const char*>(result.data.data()),
              static_cast<std::streamsize>(result.data.size()));
      break;
    case brscan::PixelFormat::kGray:
      f << "P5\n" << result.width << " " << result.height << "\n255\n";
      f.write(reinterpret_cast<const char*>(result.data.data()),
              static_cast<std::streamsize>(result.data.size()));
      break;
    case brscan::PixelFormat::kBitonal:
      f << "P4\n" << result.width << " " << result.height << "\n";
      f.write(reinterpret_cast<const char*>(result.data.data()),
              static_cast<std::streamsize>(result.data.size()));
      break;
  }

  if (!f) {
    std::cerr << "Error writing '" << path << "'\n";
    return false;
  }
  return true;
}

}  // namespace brscan::cli
