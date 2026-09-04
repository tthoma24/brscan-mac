#pragma once

#include <string>

#include "brscan/types.h"
#include "output_writer.h"

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

  // Per-FUNC output settings (see output_writer.h): the file format each
  // destination writes its scanned pages as, plus TIFF compression and
  // document separation. OutputSettings's own defaults (native format,
  // LZW, combine-all) are this project's default for every FUNC absent a
  // config override. The `searchable` flag is never set from the config
  // file (Task 1c.2b's OCR action sets it on the caller side); it stays at
  // its false default here.
  OutputSettings file_output;
  OutputSettings image_output;
  OutputSettings ocr_output;
  OutputSettings email_output;

  // Per-FUNC paper-size token (see daemon/paper_size.h's AreaForPaper),
  // e.g. "LETTER". Stored as the raw `P=`-style token, not validated
  // against daemon/paper_size.h's table and not turned into a
  // brscan::Area here -- that mapping, and any precedence against the
  // scan button's own config-command paper token, is a later task's job
  // (Task 1d.4). Empty means "no explicit paper configured".
  std::string file_paper;
  std::string image_paper;
  std::string ocr_paper;
  std::string email_paper;

  // IMAGE-destination action setting (see daemon/actions.cpp's
  // PerformImageAction): the app name passed to `open -a <image_app>`
  // when opening a saved scan. Empty means no `-a` flag at all -- `open`
  // then launches the file's default app for its extension.
  std::string image_app;

  // EMAIL-destination action setting (see daemon/actions.cpp's
  // PerformEmailAction): the address a freshly composed outgoing Mail.app
  // message is pre-addressed to. Empty leaves the message's To: field
  // blank for the user to fill in -- the message is never sent
  // automatically either way.
  std::string email_to;
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
//   image_app           app name for the IMAGE destination's `open -a`
//                        (empty: use the file's default app)
//   email_to            recipient address the EMAIL destination's
//                        outgoing Mail message is pre-addressed to
//                        (empty: leave To: blank)
//   <dest>.mode         color | gray | bw | errdiff | truegray
//   <dest>.dpi          positive integer, sets both x_dpi and y_dpi
//   <dest>.source       flatbed | adf | adf-duplex
//   <dest>.format       pdf | tiff | jpeg | png | native
//                        (default native; see output_writer.h)
//   <dest>.tiff_compression
//                        lzw | g3 | g4 (default lzw; only affects TIFF)
//   <dest>.separation   combine | every:N (N a positive int; default
//                        combine -- one container for all pages)
//   <dest>.paper        LETTER | LEGAL | A4 | LEDGER | A3 | A5 | EXECUTIVE |
//                        PHOTO | BCARD (default empty -- no explicit
//                        paper; stored raw, not validated here -- see
//                        daemon/paper_size.h)
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

// The OutputSettings to use for a button event's FUNC (FILE/IMAGE/OCR/
// EMAIL, matched case-sensitively per the wire protocol -- see
// daemon/button_listener.h's ButtonEvent::func). Returns cfg.file_output
// for any other string, mirroring ParamsForFunc's FILE-as-safe-fallback
// behavior.
const OutputSettings& OutputSettingsForFunc(const Config& cfg,
                                            const std::string& func);

// The raw `<dest>.paper` token configured for a button event's FUNC
// (FILE/IMAGE/OCR/EMAIL, matched case-sensitively per the wire protocol --
// see daemon/button_listener.h's ButtonEvent::func). Returns
// cfg.file_paper for any other string, mirroring ParamsForFunc's
// FILE-as-safe-fallback behavior. May be empty (no explicit paper
// configured); this accessor does not validate or map the token to an
// area -- see daemon/paper_size.h for that.
const std::string& PaperForFunc(const Config& cfg, const std::string& func);

// True if `func` is one of the four known FUNCs (kFuncFile/kFuncImage/
// kFuncOcr/kFuncEmail above), false for anything else. `func` comes
// straight off an untrusted UDP notification (see
// daemon/button_listener.h's ParseNotification), so daemon/
// handle_event.cpp's HandleButtonEvent uses this to reject a forged or
// corrupted FUNC outright -- before it's used for anything, in
// particular before it ever reaches a file path.
bool IsKnownFunc(const std::string& func);

}  // namespace brscan::scand
