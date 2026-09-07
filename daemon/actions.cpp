#include "actions.h"

#include <spawn.h>
#include <sys/wait.h>

#include <filesystem>
#include <iostream>
#include <sstream>
#include <system_error>

extern "C" char** environ;

namespace brscan::scand {

namespace {

// Escapes `s` for use inside a double-quoted AppleScript string literal:
// backslash and double-quote are the only two characters AppleScript
// string literals treat specially, so both get a backslash prepended.
// (AppleScript strings have no other escape sequences to worry about --
// unlike a shell, there is no word-splitting or globbing to defend
// against here; this exists purely so an embedded '"' or '\' in a file
// path or configured recipient can't prematurely close the string or
// otherwise break the script's syntax.)
std::string EscapeForAppleScriptString(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// Builds the AppleScript run by the EMAIL action: a new outgoing Mail.app
// message with every path in `attachments` attached (in order), addressed
// to `cfg.email_to` if set, brought to the front -- and, deliberately,
// never sent. There is no `send` (or `save`) statement anywhere in this
// script; PerformAction for EMAIL leaves the message open for the user to
// review before they send it themselves.
std::string BuildEmailAppleScript(const std::vector<std::string>& attachments,
                                   const Config& cfg) {
  std::ostringstream script;
  script << "tell application \"Mail\"\n"
         << "  set newMessage to make new outgoing message with properties "
            "{subject:\"Scanned Document\", visible:true}\n"
         << "  tell newMessage\n";
  for (const std::string& path : attachments) {
    const std::string escaped_path = EscapeForAppleScriptString(path);
    script << "    make new attachment with properties {file name:POSIX "
              "file \""
           << escaped_path << "\"} at after the last paragraph\n";
  }
  if (!cfg.email_to.empty()) {
    const std::string escaped_to = EscapeForAppleScriptString(cfg.email_to);
    script << "    make new to recipient at end of to recipients with "
              "properties {address:\""
           << escaped_to << "\"}\n";
  }
  script << "  end tell\n"
         << "  activate\n"
         << "end tell\n";
  return script.str();
}

// Opens every path in `written` with a single `/usr/bin/open` invocation
// (macOS `open` accepts multiple files and opens each in turn) -- matching
// the manufacturer driver's Scan-to-Image, which opens every per-page JPEG
// it produces rather than just the first.
Status PerformImageAction(const std::vector<std::string>& written,
                           const Config& cfg, const CommandRunner& runner) {
  std::vector<std::string> argv = {"/usr/bin/open"};
  if (!cfg.image_app.empty()) {
    argv.push_back("-a");
    argv.push_back(cfg.image_app);
  }
  argv.insert(argv.end(), written.begin(), written.end());

  const int rc = runner(argv);
  if (rc != 0) {
    std::cerr << "[actions] IMAGE: '/usr/bin/open' exited with status " << rc
               << " for " << written.size()
               << (written.size() == 1 ? " file\n" : " files\n");
    return Status::kIoError;
  }
  std::cout << "[actions] IMAGE: opened " << written.size()
             << (written.size() == 1 ? " file" : " files");
  if (!cfg.image_app.empty()) std::cout << " with " << cfg.image_app;
  std::cout << " (" << written.front();
  for (size_t i = 1; i < written.size(); ++i) std::cout << ", " << written[i];
  std::cout << ")\n";
  return Status::kOk;
}

// EMAIL attaches every file in `written` to a new, unsent Mail draft and
// then removes those files, so an EMAIL press leaves no persistent copy on
// disk (issue #20). HandleButtonEvent writes EMAIL's scan to a private
// temporary directory (not save_dir) precisely so this removal is safe --
// FILE/IMAGE/OCR, whose written file *is* the deliverable, never reach this
// function. Mail copies each attachment into the composed message
// synchronously as the AppleScript adds it, so the on-disk source is no
// longer needed once osascript returns success.
Status PerformEmailAction(const std::vector<std::string>& written,
                           const Config& cfg, const CommandRunner& runner) {
  const std::string script = BuildEmailAppleScript(written, cfg);
  const std::vector<std::string> argv = {"/usr/bin/osascript", "-e", script};

  const int rc = runner(argv);
  if (rc != 0) {
    // Attaching failed. Deliberately keep the temp file(s) rather than
    // silently dropping the scan, and report where they are so the scan
    // isn't lost -- HandleButtonEvent leaves the temp directory in place on
    // this failure path for the same reason.
    std::cerr << "[actions] EMAIL: '/usr/bin/osascript' exited with status "
               << rc << "; keeping the scan file(s) at:\n";
    for (const std::string& path : written) {
      std::cerr << "  " << path << "\n";
    }
    return Status::kIoError;
  }

  // Success: Mail now holds the attachment(s); remove the temp source(s).
  for (const std::string& path : written) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
      std::cerr << "[actions] EMAIL: could not remove temp attachment '"
                 << path << "': " << ec.message() << "\n";
    }
  }
  std::cout << "[actions] EMAIL: attached " << written.size()
             << (written.size() == 1 ? " file" : " files")
             << " to a new Mail draft (left unsent, no copy kept)\n";
  return Status::kOk;
}

}  // namespace

int DefaultCommandRunner(const std::vector<std::string>& argv) {
  if (argv.empty()) return -1;

  std::vector<char*> c_argv;
  c_argv.reserve(argv.size() + 1);
  for (const std::string& arg : argv) {
    c_argv.push_back(const_cast<char*>(arg.c_str()));
  }
  c_argv.push_back(nullptr);

  pid_t pid = 0;
  const int spawn_rc =
      posix_spawn(&pid, argv[0].c_str(), nullptr, nullptr, c_argv.data(), environ);
  if (spawn_rc != 0) return -1;

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
}

Status PerformAction(const std::string& func,
                      const std::vector<std::string>& written,
                      const Config& cfg, const CommandRunner& runner) {
  if (func == kFuncFile) {
    for (const std::string& path : written) {
      std::cout << "[actions] FILE: scan saved to " << path << "\n";
    }
    return Status::kOk;
  }

  if (func == kFuncImage) {
    if (written.empty()) {
      std::cerr << "[actions] IMAGE: no output file to open\n";
      return Status::kIoError;
    }
    return PerformImageAction(written, cfg, runner);
  }
  if (func == kFuncOcr) {
    // WriteConfiguredOutput already produced the searchable PDF (see
    // daemon/handle_event.cpp, which forces OCR's OutputSettings to
    // PDF+searchable before calling it) -- nothing left to do here but log
    // it.
    for (const std::string& path : written) {
      std::cout << "[actions] OCR: searchable PDF at " << path << "\n";
    }
    return Status::kOk;
  }
  if (func == kFuncEmail) return PerformEmailAction(written, cfg, runner);

  std::cout << "[actions] unrecognized FUNC '" << func
             << "'; treating as no-op\n";
  return Status::kOk;
}

Status PerformAction(const std::string& func,
                      const std::vector<std::string>& written,
                      const Config& cfg) {
  return PerformAction(func, written, cfg, DefaultCommandRunner);
}

}  // namespace brscan::scand
