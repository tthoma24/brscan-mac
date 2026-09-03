#pragma once

#include <string>

#include "actions.h"
#include "brscan/transport.h"
#include "brscan/types.h"
#include "button_listener.h"
#include "config.h"

// The per-button-press pipeline: FUNC -> Params -> RunScan -> save ->
// PerformAction. Split out from tools/brscan-scand.cpp (which owns the
// event loop, SNMP re-registration, and the real TcpTransport) so it can
// be exercised hermetically against a brscan::FakeTransport in tests.
namespace brscan::scand {

// The file extension tools/scan_output.h's WriteOutput() produces for
// `format`: "jpg" for a baseline-JPEG color result (PixelFormat::kRgb),
// "pgm" for a binary-PGM gray result (PixelFormat::kGray), "pbm" for a
// binary-PBM 1-bit result (PixelFormat::kBitonal).
std::string ExtensionForFormat(brscan::PixelFormat format);

// Builds the path HandleButtonEvent writes a scan to, under `save_dir`: a
// timestamped filename that also carries `event.func` and `event.regid`,
// so two button presses landing in the same wall-clock second (or a clock
// that jumps backward) still can't collide, followed by the extension for
// `format`.
//
// SECURITY: `event.func` and `event.regid` come straight off an untrusted
// UDP notification (see daemon/button_listener.h's ParseNotification),
// which does not itself restrict their charset. This function strips
// anything but ASCII letters/digits/'_'/'-' from both before using them
// in a path component, so a forged datagram (e.g. REGID=
// "../../../../Library/LaunchAgents/x") cannot escape `save_dir` via
// this path. HandleButtonEvent independently re-validates `event.func`
// against the known FUNC set before ever calling this (see
// config.h's IsKnownFunc), and re-checks the fully-built path stays
// under `save_dir` after this returns -- see HandleButtonEvent's own doc
// comment below for that second, independent layer.
std::string BuildOutputPath(const std::string& save_dir,
                             const ButtonEvent& event,
                             brscan::PixelFormat format);

// Handles one already-ACKed button-press notification end to end:
// rejects `event.func` outright (Status::kProtocolError, no scan run) if
// it isn't one of the four known FUNCs (config.h's IsKnownFunc) --
// defense against a forged or corrupted notification, since an unknown
// FUNC has no safe Params to scan with and its raw text must never reach
// a file path unvalidated. Otherwise resolves `event.func` to Params via
// ParamsForFunc(cfg, ...), runs RunScan over `transport` (which, for the
// document feeder, may return more than one page -- see
// libbrscan/scanner.h), writes every page under cfg.save_dir via
// BuildOutputPath()/tools/scan_output.h's WritePages() -- refusing to
// write (Status::kIoError) if the base path, once resolved, does not
// actually land inside cfg.save_dir, as a second independent check on top
// of BuildOutputPath()'s own sanitization -- and dispatches
// PerformAction() on page 1's *actual* file (tools/scan_output.h's
// PagePath(base, 1, page_count) -- the base path itself for a single-page
// scan, since WritePages never numbers a lone page, or the `-001`-suffixed
// file for a multi-page one, since WritePages never writes the bare base
// path when there's more than one page). TODO(Task 1c.2): drive
// PerformAction from the full multi-page/PDF output instead of page 1
// alone. `transport` must already be Transport::Connect()ed; this function
// neither connects nor disconnects it (matching RunScan's own contract) so
// the caller controls the connection's lifetime.
//
// On success, sets `*saved_path` to that same page-1 file (never the
// unwritten base path for a multi-page scan) and returns whatever
// PerformAction() returned (Status::kOk today; see daemon/actions.h).
// `*saved_path` is left untouched on failure. Returns RunScan's status
// unchanged if the scan itself failed (including Status::kProtocolError
// if RunScan reported success with zero pages -- not expected, but
// guarded rather than assumed), or Status::kIoError if the scan succeeded
// but a page could not be written (including the save_dir-escape refusal
// above).
//
// This overload runs PerformAction()'s IMAGE/EMAIL external commands
// through DefaultCommandRunner (see daemon/actions.h) -- i.e. this is the
// real, production behavior: an IMAGE-destination press really does spawn
// `/usr/bin/open` on the saved file, an EMAIL-destination press really
// does spawn `/usr/bin/osascript` against Mail.app.
Status HandleButtonEvent(const ButtonEvent& event, const Config& cfg,
                          brscan::Transport& transport,
                          std::string* saved_path);

// Same as above, but runs PerformAction()'s external commands through
// `runner` instead of DefaultCommandRunner. Tests that exercise this
// full FUNC -> scan -> save -> action pipeline (as opposed to
// PerformAction in isolation -- see tests/actions_test.cpp) must use
// this overload with a fake runner: an IMAGE- or EMAIL-destination
// ButtonEvent driven through the other, no-runner-argument overload
// will, in production fashion, actually spawn `/usr/bin/open` or
// `/usr/bin/osascript` -- e.g. actually launching Preview.app on
// whatever file the test just wrote to a real temp directory.
Status HandleButtonEvent(const ButtonEvent& event, const Config& cfg,
                          brscan::Transport& transport,
                          std::string* saved_path, const CommandRunner& runner);

}  // namespace brscan::scand
