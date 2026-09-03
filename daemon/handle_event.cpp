#include "handle_event.h"

#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>
#include <vector>

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

  // Bound the wire-supplied contributions so the assembled filename can't
  // exceed the filesystem's per-component limit (NAME_MAX, 255 on macOS).
  // REGID in particular arrives off the network and could be as large as
  // the whole datagram; without this cap a long REGID would build a
  // >255-byte name that fails to open with ENAMETOOLONG. The timestamp and
  // fixed text keep the full name well under NAME_MAX once these are
  // bounded.
  constexpr size_t kMaxFuncChars = 32;
  constexpr size_t kMaxRegidChars = 64;
  if (safe_func.size() > kMaxFuncChars) safe_func.resize(kMaxFuncChars);
  if (safe_regid.size() > kMaxRegidChars) safe_regid.resize(kMaxRegidChars);

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
  return HandleButtonEvent(event, cfg, transport, saved_path,
                            DefaultCommandRunner);
}

Status HandleButtonEvent(const ButtonEvent& event, const Config& cfg,
                          brscan::Transport& transport,
                          std::string* saved_path, const CommandRunner& runner) {
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

  std::vector<brscan::ScanResult> pages;
  const Status scan_status = brscan::RunScan(transport, params, &pages);
  if (scan_status != Status::kOk) {
    std::cerr << "[handle_event] FUNC=" << event.func
               << ": scan failed: "
               << brscan::cli::DescribeFailure(scan_status) << "\n";
    return scan_status;
  }
  // RunScan only returns kOk after pushing at least one page (see
  // scanner.cpp's page loop); an empty vector here would mean this
  // invariant broke somewhere upstream. Guard it explicitly rather than
  // indexing pages[0] below on a vector that might be empty.
  if (pages.empty()) {
    std::cerr << "[handle_event] FUNC=" << event.func
               << ": scan reported success with no pages\n";
    return Status::kProtocolError;
  }
  std::cout << "[handle_event] FUNC=" << event.func << ": scan complete ("
             << pages.size() << (pages.size() == 1 ? " page, " : " pages, ")
             << pages[0].width << "x" << pages[0].height << ")\n";

  // Best-effort: if save_dir already exists (the common case after the
  // first scan) this is a no-op; if it can't be created, WriteOutput below
  // will fail to open the file and report that instead.
  std::error_code ec;
  std::filesystem::create_directories(cfg.save_dir, ec);

  const std::string path =
      BuildOutputPath(cfg.save_dir, event, pages[0].format);

  // Second, independent check on top of BuildOutputPath()'s own
  // sanitization (see its doc comment in handle_event.h): confirm the
  // fully-built, resolved path actually lands inside save_dir before
  // writing anything there.
  if (!IsPathWithinDirectory(path, cfg.save_dir)) {
    std::cerr << "[handle_event] refusing to write outside save_dir: '"
               << path << "' is not under '" << cfg.save_dir << "'\n";
    return Status::kIoError;
  }

  // Resolve this FUNC's configured output format/separation (see
  // daemon/config.h's OutputSettingsForFunc and daemon/output_writer.h).
  OutputSettings settings = OutputSettingsForFunc(cfg, event.func);
  if (event.func == kFuncOcr) {
    // OCR's deliverable is always a searchable PDF: "native" wouldn't be
    // searchable, so a configured (or default) native format is promoted
    // to PDF here; any other explicitly configured container format
    // (tiff/jpeg/png) is left alone, but `searchable` only ever means
    // anything for a PDF page (see output_writer.h), so it's only set
    // when the format actually is PDF.
    if (settings.format == OutputFormat::kNative) {
      settings.format = OutputFormat::kPdf;
    }
    settings.searchable = (settings.format == OutputFormat::kPdf);
  }

  std::vector<std::string> written;
  const Status write_status =
      WriteConfiguredOutput(pages, settings, path, &written);
  if (write_status != Status::kOk) {
    std::cerr << "[handle_event] FUNC=" << event.func
               << ": failed to write configured output for '" << path
               << "'\n";
    return write_status;
  }
  if (written.empty()) {
    std::cerr << "[handle_event] FUNC=" << event.func
               << ": WriteConfiguredOutput reported success with no files "
                  "written\n";
    return Status::kIoError;
  }

  // Third containment check, alongside BuildOutputPath()'s own
  // sanitization and the base-path check above: WriteConfiguredOutput
  // derives every path it returns from `path`'s own directory and stem
  // (replacing the extension, or adding a `-NNN`/`-docNNN` suffix -- see
  // output_writer.h), never from event.func/event.regid directly, but
  // `path` itself is still built from those untrusted fields, so
  // re-validate each returned path rather than trusting that derivation
  // blindly.
  for (const std::string& file : written) {
    if (!IsPathWithinDirectory(file, cfg.save_dir)) {
      std::cerr << "[handle_event] refusing to act on output outside "
                    "save_dir: '"
                 << file << "' is not under '" << cfg.save_dir << "'\n";
      return Status::kIoError;
    }
  }

  *saved_path = written.front();
  if (written.size() == 1) {
    std::cout << "[handle_event] FUNC=" << event.func << ": wrote "
               << written.front() << "\n";
  } else {
    std::cout << "[handle_event] FUNC=" << event.func << ": wrote "
               << written.size() << " files, starting at " << written.front()
               << "\n";
  }

  return PerformAction(event.func, written, cfg, runner);
}

}  // namespace brscan::scand
