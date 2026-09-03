#include "scan_output.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace brscan::cli {

namespace {

// Inserts a "-<NNN>" page number (1-based, zero-padded to 3 digits) before
// `path`'s extension, e.g. NumberedPagePath("scan.jpg", 1) == "scan-001.jpg".
// A dot is only treated as the extension separator if it falls after the
// last path separator (a dot in a directory name isn't an extension); if
// `path` has no extension, the suffix is appended at the end instead.
std::string NumberedPagePath(const std::string& path, int page_number) {
  std::ostringstream suffix;
  suffix << '-' << std::setfill('0') << std::setw(3) << page_number;

  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    return path.substr(0, dot) + suffix.str() + path.substr(dot);
  }
  return path + suffix.str();
}

}  // namespace

std::string PagePath(const std::string& base, int index_1based, int total) {
  if (total <= 1) return base;
  return NumberedPagePath(base, index_1based);
}

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

bool WritePages(const std::vector<brscan::ScanResult>& pages,
                 const std::string& path) {
  bool ok = true;
  const int total = static_cast<int>(pages.size());
  for (int i = 0; i < total; ++i) {
    const std::string page_path = PagePath(path, i + 1, total);
    if (!WriteOutput(pages[static_cast<size_t>(i)], page_path)) ok = false;
  }
  return ok;
}

}  // namespace brscan::cli
