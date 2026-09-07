#pragma once

#include <optional>
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
  // brscan::Area here -- daemon/button_plan.h's PlanButtonScan does that,
  // and only takes this field's paper into account at all when the scan
  // button's own config-command didn't itself carry an explicit paper
  // (Touch-Panel-OFF; see docs/BUTTON.md's "Touch-Panel precedence").
  // Empty means "no explicit paper configured".
  std::string file_paper;
  std::string image_paper;
  std::string ocr_paper;
  std::string email_paper;

  // Per-FUNC remove-background level (see reference/protocol-notes-button-
  // options.md's decode of the button config command's G=/L=): 0 (off,
  // the default), 64 (low), 128 (medium), or 192 (high). Mirrors
  // file_paper/image_paper/etc.'s raw-per-dest-field pattern above --
  // mapped to brscan::Params::remove_background(bool)/
  // remove_background_level(int) only in daemon/button_plan.cpp's
  // Touch-Panel-OFF branch (Touch-Panel-ON stays authoritative from the
  // printer's own config command's G=/L=; see PlanButtonScan's header
  // comment).
  int file_remove_background_level = 0;
  int image_remove_background_level = 0;
  int ocr_remove_background_level = 0;
  int email_remove_background_level = 0;

  // Per-FUNC ADF high-speed toggle (see reference/protocol-notes-button-
  // options.md's decode of the button config command's X=): the OFF-path
  // counterpart of the LCD's own high-speed setting. When set, the device
  // feeds pages landscape for throughput and returns them rotated 90
  // degrees, so daemon/handle_event.cpp rotates each page back to portrait
  // (daemon/image_transform.h) before writing. Mirrors
  // file_remove_background_level/etc.'s raw-per-dest-field pattern;
  // consulted only in daemon/button_plan.cpp's Touch-Panel-OFF branch
  // (Touch-Panel-ON stays authoritative from the printer's own config
  // command's X=). Defaults to false (off).
  bool file_high_speed = false;
  bool image_high_speed = false;
  bool ocr_high_speed = false;
  bool email_high_speed = false;

  // Per-FUNC skip-blank toggle (see reference/protocol-notes-button-
  // options.md's decode of the button config command's W=): the OFF-path
  // counterpart of the LCD's own skip-blank-page setting. When set,
  // daemon/handle_event.cpp drops the pages daemon/blank_detect.h's
  // IsBlankPage judges empty before writing. Unlike the device toggles this
  // is purely host-side -- W= is config-only, never carried in ESC X, so the
  // device never drops blanks itself. Mirrors file_high_speed/etc.'s
  // raw-per-dest-field pattern; consulted only in daemon/button_plan.cpp's
  // Touch-Panel-OFF branch (Touch-Panel-ON stays authoritative from the
  // printer's own config command's W=). Defaults to false (off).
  bool file_skip_blank = false;
  bool image_skip_blank = false;
  bool ocr_skip_blank = false;
  bool email_skip_blank = false;

  // OCR-destination output sub-format (see daemon/action_ocr.h's
  // OcrTextFormat and output_writer.h's OutputFormat text sinks): the file
  // the OCR destination produces when its scan-button `T=` sub-format is
  // NOT panel-supplied (Touch-Panel-OFF). kPdf (the default) is the
  // searchable PDF OCR has always produced; kText/kHtml/kRtf write the
  // recognized text as a .txt/.html/.rtf file instead. OCR-only, so a
  // single Config field read directly by daemon/button_plan.cpp is enough
  // -- there is no per-FUNC accessor. Touch-Panel-ON ignores this field and
  // uses the printer's own config command's `T=` token instead (see
  // PlanButtonScan's header comment).
  OutputFormat ocr_text_format = OutputFormat::kPdf;

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
//   <dest>.separation   combine | off | image:N | page:N (N a positive
//                        int; default combine -- one container for all
//                        pages). image:N starts a new document every N
//                        single-sided images; page:N starts a new document
//                        every N pages (see output_writer.h's
//                        OutputSeparation for why these currently behave
//                        identically). `every:N` is accepted as a
//                        backward-compat alias for `image:N`.
//   <dest>.paper        LETTER | LEGAL | A4 | LEDGER | A3 | A5 | EXECUTIVE |
//                        PHOTO | BCARD (default empty -- no explicit
//                        paper; stored raw, not validated here -- see
//                        daemon/paper_size.h)
//   <dest>.remove_background
//                        off | low | medium | high (default off; low=64,
//                        medium=128, high=192 -- the level daemon/
//                        button_plan.cpp's Touch-Panel-OFF branch sets
//                        brscan::Params::remove_background_level to;
//                        Touch-Panel-ON ignores this key and uses the
//                        printer's own config command's G=/L= instead)
//   ocr.ocr_format       pdf | txt | html | rtf (default pdf -- the
//                        searchable PDF OCR has always produced). OCR-only:
//                        the sub-format the OCR destination writes when the
//                        scan-button config command didn't supply its own
//                        `T=` (Touch-Panel-OFF). txt/html/rtf write the
//                        Vision-recognized text as a .txt/.html/.rtf file
//                        instead of a PDF; Touch-Panel-ON ignores this key
//                        and uses the printer's own `T=` token.
//   <dest>.high_speed    on | off (default off). The OFF-path counterpart of
//                        the LCD's own ADF high-speed setting: when on, the
//                        daemon rotates each landscape-fed page back to
//                        portrait (see daemon/image_transform.h).
//                        Touch-Panel-ON ignores this key and uses the
//                        printer's own config command's X= instead.
//   <dest>.skip_blank    on | off (default off). The OFF-path counterpart of
//                        the LCD's own skip-blank-page setting: when on, the
//                        daemon drops the pages daemon/blank_detect.h's
//                        IsBlankPage judges empty before writing.
//                        Touch-Panel-ON ignores this key and uses the
//                        printer's own config command's W= instead.
// where <dest> is one of file, image, ocr, email.
Config ParseConfig(const std::string& text);

// Reads `path` and parses it with ParseConfig(). Returns DefaultConfig()
// unchanged (not an error) if `path` does not exist or cannot be opened --
// a missing config file is the expected first-run state.
Config LoadConfig(const std::string& path);

// The daemon's default config file path: ~/.config/brscan-scand.conf.
std::string DefaultConfigPath();

// Attempts a SIGHUP-triggered config reload (see tools/brscan-scand.cpp's
// main loop): re-reads `path` via the same LoadConfig() startup uses, and
// returns the freshly parsed Config -- unless it would be unusable (an
// empty printer_host: see kDefaultPrinterHost above), in which case this
// returns std::nullopt so the caller keeps its previous Config in place
// rather than swapping to a broken one. A `path` that doesn't exist at
// reload time (e.g. deleted, or briefly absent mid-rewrite) is treated the
// same way: LoadConfig() would return DefaultConfig() (empty
// printer_host), which this function also rejects.
//
// Factored out of tools/brscan-scand.cpp's signal plumbing so the reload
// decision itself -- fresh Config vs. keep-previous -- is unit-testable
// without a real SIGHUP or process (see tests/config_reload_test.cpp).
std::optional<Config> TryReloadConfig(const std::string& path);

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

// The configured `<dest>.remove_background` level (0/64/128/192; see
// Config::file_remove_background_level above) for a button event's FUNC
// (FILE/IMAGE/OCR/EMAIL, matched case-sensitively per the wire protocol --
// see daemon/button_listener.h's ButtonEvent::func). Returns
// cfg.file_remove_background_level for any other string, mirroring
// ParamsForFunc's FILE-as-safe-fallback behavior. 0 means off (no
// `<dest>.remove_background` configured, or explicitly configured off).
int RemoveBackgroundLevelForFunc(const Config& cfg, const std::string& func);

// The configured `<dest>.high_speed` toggle (see Config::file_high_speed
// above) for a button event's FUNC (FILE/IMAGE/OCR/EMAIL, matched
// case-sensitively per the wire protocol -- see daemon/button_listener.h's
// ButtonEvent::func). Returns cfg.file_high_speed for any other string,
// mirroring ParamsForFunc's FILE-as-safe-fallback behavior. False means off
// (no `<dest>.high_speed` configured, or explicitly configured off).
bool HighSpeedForFunc(const Config& cfg, const std::string& func);

// The configured `<dest>.skip_blank` toggle (see Config::file_skip_blank
// above) for a button event's FUNC (FILE/IMAGE/OCR/EMAIL, matched
// case-sensitively per the wire protocol -- see daemon/button_listener.h's
// ButtonEvent::func). Returns cfg.file_skip_blank for any other string,
// mirroring ParamsForFunc's FILE-as-safe-fallback behavior. False means off
// (no `<dest>.skip_blank` configured, or explicitly configured off).
bool SkipBlankForFunc(const Config& cfg, const std::string& func);

// True if `func` is one of the four known FUNCs (kFuncFile/kFuncImage/
// kFuncOcr/kFuncEmail above), false for anything else. `func` comes
// straight off an untrusted UDP notification (see
// daemon/button_listener.h's ParseNotification), so daemon/
// handle_event.cpp's HandleButtonEvent uses this to reject a forged or
// corrupted FUNC outright -- before it's used for anything, in
// particular before it ever reaches a file path.
bool IsKnownFunc(const std::string& func);

}  // namespace brscan::scand
