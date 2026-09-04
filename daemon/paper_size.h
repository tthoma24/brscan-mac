#pragma once

#include <optional>
#include <string>

#include "brscan/types.h"

// A clean-room table mapping a Brother scan-button config command's `P=`
// paper token (see daemon/button_config.h's ButtonConfig::paper and
// docs/BUTTON.md's "Config command" section) to the brscan::Area the
// printer actually scans for that paper size, at a given dpi.
//
// This is a self-contained data/lookup unit only: it does not decide when
// to apply a paper size, and nothing in this project yet calls
// AreaForPaper() with a config-file or button-config token (that mapping,
// and any precedence between an explicit `<dest>.paper` config key and the
// button config's own P=, are later Plan 1d tasks -- see
// daemon/config.h's `<dest>.paper` key for the config side of this).
namespace brscan::scand {

// Returns the scan area for `paper_token` (an exact, case-sensitive match
// against one of the 9 tokens below -- Brother always sends upper case) at
// `dpi`, or std::nullopt if the token is unknown or `dpi` is not positive.
// The caller falls back to some other area (e.g. the printer's own
// ESC I offer) when this returns nullopt.
std::optional<brscan::Area> AreaForPaper(const std::string& paper_token,
                                          int dpi);

// True if `paper_token` names one of the 9 known paper sizes below (exact,
// case-sensitive match), false for anything else -- including B4, which
// Brother's protocol supports elsewhere but the LCD panel this project
// targets never offers as a Scan-button paper choice.
bool IsKnownPaper(const std::string& paper_token);

}  // namespace brscan::scand
