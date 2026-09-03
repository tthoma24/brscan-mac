// brscan-cli: run one scan against a Brother network scanner and write the
// result to a file. See docs/PROTOCOL.md for the wire protocol and
// libbrscan/scanner.h for the scan flow this drives.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#include "brscan/scanner.h"
#include "brscan/transport_tcp.h"
#include "brscan/types.h"

namespace {

constexpr uint16_t kDefaultPort = 54921;
constexpr int kDefaultResolution = 300;

void PrintUsage(const char* argv0, std::ostream& out) {
  out
      << "Usage: " << argv0 << " --host HOST --output PATH [options]\n"
      << "\n"
      << "Required:\n"
      << "  --host HOST         Scanner hostname or IP address.\n"
      << "  --output PATH       Where to write the scanned image.\n"
      << "\n"
      << "Options:\n"
      << "  --port PORT         TCP port (default " << kDefaultPort << ").\n"
      << "  --mode MODE         color | gray | bw | errdiff | truegray\n"
      << "                      (default color).\n"
      << "  --resolution DPI    Scan resolution in dpi (default "
      << kDefaultResolution << ").\n"
      << "  --source SOURCE     flatbed | adf | adf-duplex (default flatbed).\n"
      << "  --area X0,Y0,X1,Y1  Scan area in pixels at the scan resolution\n"
      << "                      (default: the full area the device offers).\n"
      << "\n"
      << "Output format follows --mode: color writes a JPEG; gray and\n"
      << "truegray write a binary PGM (P5); bw and errdiff (1-bit modes)\n"
      << "write a binary PBM (P4).\n";
}

struct Args {
  std::string host;
  uint16_t port = kDefaultPort;
  brscan::ScanMode mode = brscan::ScanMode::kColor;
  int resolution = kDefaultResolution;
  brscan::Source source = brscan::Source::kFlatbed;
  bool duplex = false;
  std::optional<brscan::Area> area;
  std::string output;
  bool help = false;
};

// Parses "--flag value" pairs. Returns false (after printing an error) on
// any unrecognized flag, missing value, or malformed value. `--help`/`-h`
// is not an error: it sets `out->help` and returns true immediately,
// skipping the rest of the command line and the required-argument checks,
// so the caller can print usage and exit 0 rather than 2.
bool ParseArgs(int argc, char** argv, Args* out) {
  const auto next_value = [&](int& i) -> std::optional<std::string> {
    if (i + 1 >= argc) return std::nullopt;
    return std::string(argv[++i]);
  };

  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];

    if (flag == "--host") {
      const auto v = next_value(i);
      if (!v) { std::cerr << "--host requires a value\n"; return false; }
      out->host = *v;
    } else if (flag == "--port") {
      const auto v = next_value(i);
      if (!v) { std::cerr << "--port requires a value\n"; return false; }
      const int port = std::atoi(v->c_str());
      if (port <= 0 || port > 65535) {
        std::cerr << "--port must be an integer from 1 to 65535, got '" << *v << "'\n";
        return false;
      }
      out->port = static_cast<uint16_t>(port);
    } else if (flag == "--mode") {
      const auto v = next_value(i);
      if (!v) { std::cerr << "--mode requires a value\n"; return false; }
      if (*v == "color") {
        out->mode = brscan::ScanMode::kColor;
      } else if (*v == "gray") {
        out->mode = brscan::ScanMode::kGray;
      } else if (*v == "bw") {
        out->mode = brscan::ScanMode::kBlackWhite;
      } else if (*v == "errdiff") {
        out->mode = brscan::ScanMode::kErrorDiffusion;
      } else if (*v == "truegray") {
        out->mode = brscan::ScanMode::kTrueGray;
      } else {
        std::cerr << "--mode must be one of 'color', 'gray', 'bw', "
                      "'errdiff', or 'truegray', got '"
                   << *v << "'\n";
        return false;
      }
    } else if (flag == "--resolution") {
      const auto v = next_value(i);
      if (!v) { std::cerr << "--resolution requires a value\n"; return false; }
      out->resolution = std::atoi(v->c_str());
      if (out->resolution <= 0) {
        std::cerr << "--resolution must be a positive integer\n";
        return false;
      }
    } else if (flag == "--source") {
      const auto v = next_value(i);
      if (!v) { std::cerr << "--source requires a value\n"; return false; }
      if (*v == "flatbed") {
        out->source = brscan::Source::kFlatbed;
        out->duplex = false;
      } else if (*v == "adf") {
        out->source = brscan::Source::kAdf;
        out->duplex = false;
      } else if (*v == "adf-duplex") {
        out->source = brscan::Source::kAdf;
        out->duplex = true;
      } else {
        std::cerr << "--source must be 'flatbed', 'adf', or 'adf-duplex', got '"
                   << *v << "'\n";
        return false;
      }
    } else if (flag == "--area") {
      const auto v = next_value(i);
      if (!v) { std::cerr << "--area requires a value\n"; return false; }
      brscan::Area area{};
      const int n = std::sscanf(v->c_str(), "%d,%d,%d,%d", &area.x0, &area.y0,
                                  &area.x1, &area.y1);
      if (n != 4) {
        std::cerr << "--area must be X0,Y0,X1,Y1, got '" << *v << "'\n";
        return false;
      }
      out->area = area;
    } else if (flag == "--output") {
      const auto v = next_value(i);
      if (!v) { std::cerr << "--output requires a value\n"; return false; }
      out->output = *v;
    } else if (flag == "--help" || flag == "-h") {
      out->help = true;
      return true;
    } else {
      std::cerr << "Unrecognized argument: " << flag << "\n";
      return false;
    }
  }

  if (out->host.empty()) {
    std::cerr << "--host is required\n";
    return false;
  }
  if (out->output.empty()) {
    std::cerr << "--output is required\n";
    return false;
  }
  return true;
}

// A human-readable line for each failure Status RunScan can report, per
// libbrscan/scanner.h's doc comment.
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

// Writes `result` to `path`: the JPEG bytes as-is for color, a binary PGM
// (P5) for gray (GRAY64 raw or GRAY256/RLENGTH, both PixelFormat::kGray),
// or a binary PBM (P4) for the 1-bit modes (TEXT/ERRDIF,
// PixelFormat::kBitonal -- see the bit-packing convention documented on
// PixelFormat::kBitonal in types.h, which is exactly what P4 expects, so
// `result.data` is written out unchanged). Returns false (after printing
// an error) if the file can't be opened for writing.
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

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage(argv[0], std::cerr);
    return 2;
  }
  if (args.help) {
    PrintUsage(argv[0], std::cout);
    return 0;
  }

  brscan::Params params;
  params.mode = args.mode;
  params.source = args.source;
  params.duplex = args.duplex;
  params.x_dpi = args.resolution;
  params.y_dpi = args.resolution;
  if (args.area.has_value()) params.area = *args.area;

  brscan::TcpTransport transport(args.host, args.port);
  const brscan::Status connect_status = transport.Connect();
  if (connect_status != brscan::Status::kOk) {
    std::cerr << "Could not connect to " << args.host << ":" << args.port
              << ": " << DescribeFailure(connect_status) << "\n";
    return ExitCodeFor(connect_status) == 0 ? 1 : ExitCodeFor(connect_status);
  }

  brscan::ScanResult result;
  const brscan::Status scan_status = brscan::RunScan(transport, params, &result);
  transport.Disconnect();

  if (scan_status != brscan::Status::kOk) {
    std::cerr << "Scan failed: " << DescribeFailure(scan_status) << "\n";
    return ExitCodeFor(scan_status);
  }

  if (!WriteOutput(result, args.output)) return 1;

  std::cout << "Wrote " << args.output << " (" << result.width << "x"
            << result.height << ")\n";
  return 0;
}
