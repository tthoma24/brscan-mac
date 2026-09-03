#pragma once

#include <functional>
#include <string>
#include <vector>

#include "brscan/types.h"
#include "config.h"

// The FUNC dispatch point: what happens after a button-triggered scan has
// already been pulled and written to disk. See reference/plan-master.md's
// Plan 1b Task 5 for the full per-destination behavior implemented here.
namespace brscan::scand {

// Runs an external command given its full argv (argv[0] is the
// executable's absolute path; no PATH search and no shell involved).
// Returns the process's exit status (0 on success), or a negative value
// if the process could not even be spawned or its status could not be
// retrieved.
//
// PerformAction's IMAGE and EMAIL actions never build a shell command
// line out of untrusted input (FUNC values, file paths, or config
// strings) -- they always invoke a fixed executable with an explicit
// argv through this type, so nothing here is ever interpreted by a
// shell. DefaultCommandRunner (the production implementation) uses
// posix_spawn directly; tests inject a fake CommandRunner instead, so
// they can assert the exact argv (and, for EMAIL, the exact AppleScript
// text) without spawning `open` or Mail.app for real.
using CommandRunner = std::function<int(const std::vector<std::string>& argv)>;

// The production CommandRunner: posix_spawn(argv[0], argv...) plus
// waitpid, returning the child's exit status. argv must be non-empty.
// Returns -1 if the process could not be spawned, or exited abnormally
// (e.g. killed by a signal) rather than via a normal exit() call.
int DefaultCommandRunner(const std::vector<std::string>& argv);

// Performs the destination action for `func` (FILE/IMAGE/OCR/EMAIL) on the
// scan already saved at `saved_path` (see HandleButtonEvent in
// daemon/handle_event.h, which calls this after WriteOutput succeeds).
//
//   - FILE: a no-op. Saving the file *is* the FILE action -- by the time
//     this runs, WriteOutput has already written it to `saved_path`.
//   - IMAGE: opens `saved_path` with `/usr/bin/open` (the file's default
//     app, or `cfg.image_app` via `open -a` if set).
//   - OCR: runs Vision text recognition over `saved_path` (see
//     daemon/action_ocr.h) and writes a searchable PDF next to it (same
//     basename, `.pdf` extension). The original scanned image is kept.
//   - EMAIL: opens a new Mail.app outgoing message with `saved_path`
//     attached, addressed to `cfg.email_to` if set, and brings Mail to
//     the front -- left for the user to review and send. The message is
//     never sent automatically.
//   - Any other string: treated as a no-op (logged, kOk) rather than as
//     an error, since the scan itself already succeeded and was saved.
//
// This overload runs IMAGE's `open` and EMAIL's `osascript` through
// DefaultCommandRunner. Returns Status::kOk on success; on a destination
// action's own failure (e.g. `open` exits nonzero, or Vision's OCR
// fails), returns a Status describing that failure -- the caller
// (HandleButtonEvent) already has the saved file either way, since the
// scan and the save both happened before this runs.
Status PerformAction(const std::string& func, const std::string& saved_path,
                      const Config& cfg);

// Same as above, but runs IMAGE's and EMAIL's external commands through
// `runner` instead of DefaultCommandRunner. Used by tests to assert the
// exact argv (and AppleScript text) without spawning anything real.
Status PerformAction(const std::string& func, const std::string& saved_path,
                      const Config& cfg, const CommandRunner& runner);

}  // namespace brscan::scand
