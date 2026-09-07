#pragma once

#include "brscan/scanner.h"

// Host-side blank-page detection for the scan-button "Skip Blank Page" toggle
// (the config command's `W=1`; see reference/protocol-notes-button-options.md
// and docs/BUTTON.md). Unlike the other panel toggles, `W=` is config-only:
// it is never carried in the `ESC X` execute command, so the device never
// drops blanks itself -- the host must detect and remove them. This header
// owns that detection; daemon/handle_event.cpp applies it, dropping the pages
// this judges blank before writing.
namespace brscan::scand {

// Returns true if `page` looks blank: it is decoded (via daemon/action_ocr.h's
// CreateCGImageFromScanResult -- the one place that turns a ScanResult's
// native bytes back into a CGImage, so kRgb/kGray/kBitonal all decode exactly
// as they do elsewhere), downscaled into a small grayscale bitmap, and judged
// by its *ink coverage* -- the fraction of pixels darker than
// kInkLuminanceThreshold. Coverage below kBlankCoverageEpsilon means blank.
//
// The measure is orientation-independent, so it composes with the high-speed
// rotation regardless of order. On any decode or bitmap failure the page is
// treated as NON-blank and logged -- a page that could not be inspected is
// never dropped.
bool IsBlankPage(const brscan::ScanResult& page);

}  // namespace brscan::scand
