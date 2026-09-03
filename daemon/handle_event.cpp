#include "handle_event.h"

#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

#include "actions.h"
#include "brscan/scanner.h"
#include "scan_output.h"

namespace brscan::scand {

namespace {

// Strips everything but ASCII letters/digits/'_'/'-' from `raw`. Used to
// scrub wire-supplied fields (FUNC, REGID) before they become part of a
// file path -- see BuildOutputPath's doc comment in handle_event.h.
std::string SanitizeForFilename(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (const char c : raw) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (ok) out.push_back(c);
  }
  return out;
}

// True if `path`, once resolved, actually lives inside `dir`. Both are
// resolved with weakly_canonical (which normalizes "." / ".." and
// resolves symlinks in whatever prefix already exists, without requiring
// the full path to exist) so this catches a traversal attempt that
// somehow survived SanitizeForFilename -- e.g. a `dir` that is itself a
// symlink pointing outside the intended tree -- not just a literal ".."
// substring. Returns false (refuse) if either path can't be resolved at
// all.
bool IsPathWithinDirectory(const std::filesystem::path& path,
                            const std::filesystem::path& dir) {
  std::error_code ec;
  const auto canonical_dir = std::filesystem::weakly_canonical(dir, ec);
  if (ec) return false;
  const auto canonical_path = std::filesystem::weakly_canonical(path, ec);
  if (ec) return false;

  const auto rel = canonical_path.lexically_relative(canonical_dir);
  if (rel.empty()) return false;
  const std::string rel_str = rel.generic_string();
  // A relative path that escapes `dir` either *is* ".." (dir's direct
  // parent) or starts with "../" (further up/across).
  if (rel_str == "..") return false;
  if (rel_str.rfind("../", 0) == 0) return false;
  return true;
}

}  // namespace

std::string ExtensionForFormat(brscan::PixelFormat format) {
  switch (format) {
    case brscan::PixelFormat::kRgb:
      return "jpg";
    case brscan::PixelFormat::kGray:
      return "pgm";
    case brscan::PixelFormat::kBitonal:
      return "pbm";
  }
  return "bin";  // unreachable
}

std::string BuildOutputPath(const std::string& save_dir,
                             const ButtonEvent& event,
                             brscan::PixelFormat format) {
  const std::time_t now = std::time(nullptr);
  std::tm local_tm{};
  localtime_r(&now, &local_tm);

  // See this function's doc comment in handle_event.h: both fields come
  // straight off an untrusted UDP notification.
  std::string safe_func = SanitizeForFilename(event.func);
  if (safe_func.empty()) safe_func = "UNKNOWN";
  std::string safe_regid = SanitizeForFilename(event.regid);
  if (safe_regid.empty()) safe_regid = "0";

  std::ostringstream name;
  name << "scan-" << std::put_time(&local_tm, "%Y%m%d-%H%M%S") << "-"
       << safe_func << "-" << safe_regid << "."
       << ExtensionForFormat(format);

  std::filesystem::path path(save_dir);
  path /= name.str();
  return path.string();
}

Status HandleButtonEvent(const ButtonEvent& event, const Config& cfg,
                          brscan::Transport& transport,
                          std::string* saved_path) {
  // event.func comes straight off an untrusted UDP notification (see
  // daemon/button_listener.h's ParseNotification, which validates the
  // wire framing but not FUNC's value against any known set). Reject
  // anything but the four known FUNCs outright, before running a scan or
  // letting the value anywhere near a file path.
  if (!IsKnownFunc(event.func)) {
    std::cerr << "[handle_event] rejecting notification with unrecognized "
                  "FUNC '"
               << event.func << "'; not scanning\n";
    return Status::kProtocolError;
  }

  const brscan::Params& params = ParamsForFunc(cfg, event.func);
  std::cout << "[handle_event] FUNC=" << event.func
             << ": starting scan (dpi=" << params.x_dpi
             << " source=" << (params.source == brscan::Source::kAdf ? "adf"
                                                                       : "flatbed")
             << ")\n";

  brscan::ScanResult result;
  const Status scan_status = brscan::RunScan(transport, params, &result);
  if (scan_status != Status::kOk) {
    std::cerr << "[handle_event] FUNC=" << event.func
               << ": scan failed: "
               << brscan::cli::DescribeFailure(scan_status) << "\n";
    return scan_status;
  }
  std::cout << "[handle_event] FUNC=" << event.func
             << ": scan complete (" << result.width << "x" << result.height
             << ")\n";

  // Best-effort: if save_dir already exists (the common case after the
  // first scan) this is a no-op; if it can't be created, WriteOutput below
  // will fail to open the file and report that instead.
  std::error_code ec;
  std::filesystem::create_directories(cfg.save_dir, ec);

  const std::string path = BuildOutputPath(cfg.save_dir, event, result.format);

  // Second, independent check on top of BuildOutputPath()'s own
  // sanitization (see its doc comment in handle_event.h): confirm the
  // fully-built, resolved path actually lands inside save_dir before
  // writing anything there.
  if (!IsPathWithinDirectory(path, cfg.save_dir)) {
    std::cerr << "[handle_event] refusing to write outside save_dir: '"
               << path << "' is not under '" << cfg.save_dir << "'\n";
    return Status::kIoError;
  }

  if (!brscan::cli::WriteOutput(result, path)) {
    std::cerr << "[handle_event] FUNC=" << event.func
               << ": failed to write '" << path << "'\n";
    return Status::kIoError;
  }
  std::cout << "[handle_event] FUNC=" << event.func << ": wrote " << path
             << "\n";
  *saved_path = path;

  return PerformAction(event.func, path, cfg);
}

}  // namespace brscan::scand
