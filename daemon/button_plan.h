#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "brscan/types.h"
#include "config.h"
#include "output_writer.h"

// The scan-button precedence planner: decides whether a button press is
// driven by the printer's own LCD-set settings ("Touch-Panel-ON") or by
// this daemon's configured defaults ("Touch-Panel-OFF"), and turns that
// decision into concrete brscan::Params + OutputSettings. See
// docs/BUTTON.md's "Touch-Panel precedence" section for the user-facing
// summary of the rule this implements.
namespace brscan::scand {

// The outcome of PlanButtonScan(): everything HandleButtonEvent needs to
// run the scan and write its output, plus the precedence branch taken
// (for logging only -- callers don't need to branch on this themselves).
struct ButtonScanPlan {
  brscan::Params params;       // For RunButtonScan; button_flow is always
                                // true.
  OutputSettings output;       // For WriteConfiguredOutput.
  bool touch_panel_on = false; // Which precedence branch was taken.
};

// Decides how to scan and write a button press, given the printer's pushed
// config-command frame, the validated route, and the daemon's config.
//
// `config_frame` is the raw `0x30 <len> 0x00 <payload>` bytes the printer
// pushes right after `ESC K` (see daemon/button_config.h's
// ParseButtonConfig and docs/BUTTON.md's "Config command"). `func` is the
// already-validated FUNC (FILE/IMAGE/OCR/EMAIL -- see config.h's
// IsKnownFunc) the button event named; this is always the route used,
// regardless of precedence branch. `cfg` is the daemon's configuration.
//
// Returns std::nullopt only if `config_frame` itself cannot be parsed at
// all (ParseButtonConfig returned std::nullopt) -- the caller should treat
// that as a protocol error, matching RunButtonScan's own
// std::nullopt-callback contract.
//
// PRECEDENCE. The printer's config command is pushed in one of two forms
// (see docs/BUTTON.md):
//   - The short "Auto" form, sent when the LCD panel's own settings were
//     never explicitly opened for this press: it carries only F=/D=/E=,
//     no R= (resolution). This project's daemon config drives the scan
//     ("Touch-Panel-OFF").
//   - The full LCD-set form, sent whenever the user actually dialed in
//     settings on the panel: it always carries R= (and M=/P=/T=/etc).
//     Those settings drive the scan ("Touch-Panel-ON"), overriding the
//     daemon's configured per-FUNC settings.
// Detection: `parsed.dpi > 0` iff `R=` was present, since ParseButtonConfig
// leaves an absent key at its zero-ish default (see button_config.h) and a
// config command never legitimately carries `R=0`. This is the one signal
// this project distinguishes the two forms by.
//
// Either branch: params.duplex always comes from the parsed config's D=
// (the printer's touch panel exposes the 2-sided setting in both Touch-
// Panel modes, so it is authoritative regardless of which branch is taken
// below -- unlike E=/edge, which is not plumbed into Params at all).
//
// Touch-Panel-ON: params.mode/x_dpi/y_dpi/area/remove_background(_level)
// come from the parsed config (area via daemon/paper_size.h's
// AreaForPaper(parsed.paper, parsed.dpi)); output format comes from
// parsed.output_type.
//
// Touch-Panel-OFF: params come from ParamsForFunc(cfg, func) as-is (mode,
// dpi, brightness, contrast, source -- duplex excepted, see above), with
// the area set from PaperForFunc(cfg, func) via AreaForPaper, defaulting to
// kDefaultAutoPaper ("LETTER") when no `<dest>.paper` is configured -- Auto
// mode carries no LCD paper size, and the ADF's ESC I offer can't report
// sheet length either; output comes from OutputSettingsForFunc(cfg, func).
//
// Either branch: as a final safety net, a scan area that is still the zero
// value once the branch above has run (an unconfigured/unknown paper
// token, Touch-Panel-OFF or -ON alike) is replaced with
// AreaForPaper(kDefaultAutoPaper, params.x_dpi) -- this is what prevents
// RunButtonScan's ADF-offer fallback (whose ymax is always 0) from ever
// producing a zero-height scan area.
//
// Either branch: params.button_flow is always true, and func == kFuncOcr
// always yields output = OutputSettingsForFunc(cfg, kFuncOcr) with format
// and searchable forced to {kPdf, true} (OCR's deliverable is always a
// searchable PDF -- see docs/BUTTON.md's "Output format"), regardless of
// the LCD's own T= sub-format (TXT/HTML/RTF) -- see the deferred-fields
// note in button_plan.cpp. Every other configured OCR output setting
// (separation/separate_n/tiff_compression) is preserved from cfg.
std::optional<ButtonScanPlan> PlanButtonScan(
    const std::vector<uint8_t>& config_frame, const std::string& func,
    const Config& cfg);

}  // namespace brscan::scand
