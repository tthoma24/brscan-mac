#pragma once

#include <string>

#include "brscan/types.h"

// The daemon's configuration: where the printer is, what to call this Mac
// in the printer's Scan menu, where FILE-destination scans land, and the
// per-FUNC scan settings (mode/dpi/source) each button press uses. See
// reference/plan-master.md's Plan 1b section for the overall daemon design
// this supports.
namespace brscan::scand {

// The four Brother destination FUNCs, as carried on the wire (see
// daemon/button_listener.h's ButtonEvent::func and
// daemon/snmp_register.h's kAppNum* constants). Exposed here (rather than
// duplicated as file-local constants in config.cpp/actions.cpp) so
// IsKnownFunc() below and every caller compare against exactly the same
// four literals.
constexpr char kFuncFile[] = "FILE";
constexpr char kFuncImage[] = "IMAGE";
constexpr char kFuncOcr[] = "OCR";
constexpr char kFuncEmail[] = "EMAIL";

// There is deliberately no default `printer_host`: every device's mDNS
// name is specific to that device (it's derived from its MAC address --
// see README.md), so shipping any real one as a fallback would leak
// whichever developer's printer it came from. `printer_host` defaults to
// empty instead, and the caller (tools/brscan-scand.cpp) treats an empty
// value as a hard "not configured" error at startup rather than silently
// falling back to some other Mac's device. Find yours with:
//   dns-sd -B _scanner._tcp
// (see README.md), then set `printer_host=<name>.local` (or its IP) in
// the config file.
constexpr char kDefaultPrinterHost[] = "";

// Default FILE-destination save directory if the config omits `save_dir`.
constexpr char kDefaultSaveDir[] = "~/Scans";

// The daemon's full configuration. Every field has a usable default (see
// DefaultConfig()) except `printer_host`, which has no safe default (see
// kDefaultPrinterHost above) -- the caller (tools/brscan-scand.cpp)
// treats it as required and refuses to start if it comes back empty.
struct Config {
  std::string printer_host = kDefaultPrinterHost;
  std::string display_name;  // Defaulted to this host's name; see
                              // DefaultDisplayName().
  std::string save_dir = kDefaultSaveDir;

  // Per-FUNC scan settings. brscan::Params's own default constructor
  // already is color/300dpi/flatbed, which is this project's chosen
  // default for every FUNC (FILE, IMAGE, OCR, EMAIL alike) absent a
  // config override.
  brscan::Params file_params;
  brscan::Params image_params;
  brscan::Params ocr_params;
  brscan::Params email_params;
};

// This machine's host name (gethostname()), or "Mac" if that call fails.
// Used as Config::display_name's default when the config file doesn't set
// one.
std::string DefaultDisplayName();

// Expands a leading "~" or "~/..." in `path` to the $HOME environment
// variable. Any path not starting with "~" is returned unchanged. If `path`
// starts with "~" but $HOME is unset, `path` is returned unchanged (rather
// than guessing) since there is nothing safe to substitute.
std::string ExpandHome(const std::string& path);

// A Config with every field at its documented default and no config file
// involved. Used both for LoadConfig()'s "file missing" case and as
// ParseConfig()'s starting point, so a config that overrides only one
// field still gets defaults for the rest.
Config DefaultConfig();

// Parses `text` (a whole config file's contents) as tolerant key=value
// lines layered onto DefaultConfig():
//   - blank lines, and lines whose first non-whitespace character is '#',
//     are ignored (comments);
//   - a line with no '=' is ignored;
//   - an unrecognized key is ignored;
//   - a recognized key with a value that fails to parse (e.g. `file.dpi=
//     abc`, `file.mode=xyz`) is ignored, leaving that field at its prior
//     value.
// A malformed or partially-understood config file therefore never fails
// outright -- every field it doesn't successfully set keeps its default,
// so the daemon can still start and register.
//
// Recognized keys:
//   printer_host        printer hostname or IP (TCP 54921 / UDP 161).
//                        REQUIRED -- there is no built-in default (see
//                        kDefaultPrinterHost above); find yours with
//                        `dns-sd -B _scanner._tcp`.
//   display_name        name shown in the printer's Scan menu
//   save_dir            FILE-destination output directory (~ expanded)
//   <dest>.mode         color | gray | bw | errdiff | truegray
//   <dest>.dpi          positive integer, sets both x_dpi and y_dpi
//   <dest>.source       flatbed | adf | adf-duplex
// where <dest> is one of file, image, ocr, email.
Config ParseConfig(const std::string& text);

// Reads `path` and parses it with ParseConfig(). Returns DefaultConfig()
// unchanged (not an error) if `path` does not exist or cannot be opened --
// a missing config file is the expected first-run state.
Config LoadConfig(const std::string& path);

// The daemon's default config file path: ~/.config/brscan-scand.conf.
std::string DefaultConfigPath();

// The Params to use for a button event's FUNC (FILE/IMAGE/OCR/EMAIL,
// matched case-sensitively per the wire protocol -- see
// daemon/button_listener.h's ButtonEvent::func). Returns cfg.file_params
// for any other string, since FILE's settings are always a safe fallback.
const brscan::Params& ParamsForFunc(const Config& cfg, const std::string& func);

// True if `func` is one of the four known FUNCs (kFuncFile/kFuncImage/
// kFuncOcr/kFuncEmail above), false for anything else. `func` comes
// straight off an untrusted UDP notification (see
// daemon/button_listener.h's ParseNotification), so daemon/
// handle_event.cpp's HandleButtonEvent uses this to reject a forged or
// corrupted FUNC outright -- before it's used for anything, in
// particular before it ever reaches a file path.
bool IsKnownFunc(const std::string& func);

}  // namespace brscan::scand
