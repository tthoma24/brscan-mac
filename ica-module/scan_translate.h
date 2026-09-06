// Plan 2 Task 7 — host scan-request -> brscan::Params translation.
//
// The ICD_ScannerSetParameters callback hands the module a CFDictionary of the
// host's selections (resolution, colour mode, source, duplex, brightness,
// contrast, scan area). ICD_ScannerStart then has to turn those selections into
// a brscan::Params for a normal, host-initiated RunScan.
//
// That translation is pure arithmetic over libbrscan's own public types, so it
// is factored out here as a framework-free function operating on a plain struct
// (ScanRequest) rather than on a CFDictionary. module_main.mm reads the
// CFDictionary into a ScanRequest (thin CoreFoundation plumbing) and calls this;
// this unit is hermetically unit-testable with no Foundation, no ICADevices, and
// no device (see tests/scan_translate_test.cpp), exactly as PLAN-2-DESIGN.md's
// testing strategy prescribes for the parameter mapping.
//
// Clean-room: our own mapping over libbrscan's public brscan::Params, following
// PLAN-2-DESIGN.md's "Scan-parameter mapping" table. The pixel-type and
// functional-unit integer values are the public ImageCaptureCore client enums
// (ICScannerPixelDataType: 0=BW, 1=Gray, 2=RGB; ICScannerFunctionalUnitType:
// 0=Flatbed, 3=DocumentFeeder), the same values scan_parameters.mm advertises.

#pragma once

#include <string>

#include "brscan/types.h"

namespace brscan::ica {

// ICScannerDocumentType raw enum values (ImageCaptureCore), used as the members
// of the ICAP_SUPPORTEDSIZES capability the module advertises. The enum is
// NON-contiguous, so each value below was verified individually against the
// macOS SDK header
// System/Library/Frameworks/ImageCaptureCore.framework/Headers/
// ICScannerFunctionalUnits.h. kDocumentTypeNone is a module-internal sentinel
// (not an SDK value) meaning "no standard document type for this paper".
inline constexpr int kDocumentTypeNone = -1;
inline constexpr int kDocumentTypeDefault = 0;      // Platten; flatbed only.
inline constexpr int kDocumentTypeA4 = 1;
inline constexpr int kDocumentTypeUSLetter = 3;
inline constexpr int kDocumentTypeUSLegal = 4;
inline constexpr int kDocumentTypeA5 = 5;
inline constexpr int kDocumentTypeUSLedger = 9;
inline constexpr int kDocumentTypeUSExecutive = 10;
inline constexpr int kDocumentTypeA3 = 11;

// Maps a daemon/paper_size.cpp paper token to its ICScannerDocumentType value,
// or kDocumentTypeNone for tokens with no clean standard case (PHOTO, BCARD --
// exposed as a custom scan area only) and for unknown tokens. Pure; the single
// source of truth shared by the capability advertisement (scan_parameters.mm)
// and any reverse lookup, so the two never diverge.
int DocumentTypeForPaperToken(const std::string& token);

// Converts a host userScanArea (offset + extent, in ICAP_UNITS = pixels) to the
// corner-bounded Area{x0,y0,x1,y1} brscan uses (x0=offX, y0=offY, x1=offX+width,
// y1=offY+height). Returns true and fills `out` only for a positive rectangle
// (width > 0 && height > 0); otherwise returns false and leaves `out` untouched
// so the caller falls back to the full-area default. Pure.
bool CornersFromUserScanArea(int offset_x, int offset_y, int width, int height,
                             Area* out);

// The host's scan selection, extracted from the SetParameters CFDictionary into
// a plain, framework-free struct. Each optional field carries a `has_*` flag so
// that an absent selection falls back to the design defaults rather than to a
// zero that would mean something (e.g. a missing resolution must default to 300,
// not 0).
struct ScanRequest {
  bool has_resolution = false;
  int resolution = 0;  // dpi, applied to both axes.

  bool has_pixel_type = false;
  int pixel_type = 0;  // ICScannerPixelDataType: 0=BW, 1=Gray, 2=RGB.

  bool has_functional_unit = false;
  int functional_unit = 0;  // ICScannerFunctionalUnitType: 0=Flatbed, 3=Feeder.

  bool duplex = false;  // Only honoured for the document feeder.

  bool has_brightness = false;
  int brightness = 0;  // Host 0..100.

  bool has_contrast = false;
  int contrast = 0;  // Host 0..100.

  // Scan area in pixels at `resolution`. Honoured only when all four bounds are
  // present and describe a positive rectangle (x1 > x0 && y1 > y0); otherwise
  // the request is for the full offered area (brscan::Area{0,0,0,0}).
  bool has_area = false;
  int area_x0 = 0;
  int area_y0 = 0;
  int area_x1 = 0;
  int area_y1 = 0;
};

// TWAIN ICAP_UNITS raw value for pixels (TWUN_PIXELS). The host's live
// SetParameters trace (Plan 2 Task 11) reports ICAP_UNITS = 5, and the scan-area
// offset/extent are expressed in this unit; a non-pixel unit is not honoured for
// the area (the request falls back to the full offered area).
inline constexpr int kIcapUnitsPixels = 5;

// The raw ICAP_* scan selection the host nests inside its `userScanArea`
// dictionary (Plan 2 Task 11 live trace: one top-level `userScanArea` whose
// entries are TWAIN {type, value[, current]} sub-dicts, e.g. ICAP_XRESOLUTION,
// ICAP_YRESOLUTION, ICAP_PIXELTYPE, ICAP_BITDEPTH, ICAP_UNITS). module_main.mm
// does the CoreFoundation traversal -- reading each entry's `value` (falling
// back to `current`) into the plain integers below -- and this pure helper turns
// them into a ScanRequest, so the mapping stays hermetically unit-testable with
// no Foundation. Every field is presence-aware: an absent capability leaves the
// corresponding ScanRequest flag false, so TranslateScanParams applies the
// design default rather than a meaningful zero.
struct IcapScanSelection {
  bool has_x_resolution = false;
  int x_resolution = 0;  // ICAP_XRESOLUTION.value (dpi).
  bool has_y_resolution = false;
  int y_resolution = 0;  // ICAP_YRESOLUTION.value (dpi).

  bool has_pixel_type = false;
  int pixel_type = 0;  // ICAP_PIXELTYPE.value: 0=BW, 1=Gray, 2=RGB.

  bool has_bit_depth = false;
  int bit_depth = 0;  // ICAP_BITDEPTH.value (captured for the trace/future use).

  bool has_units = false;
  int units = 0;  // ICAP_UNITS.value; kIcapUnitsPixels (5) == pixels.

  bool has_functional_unit = false;
  int functional_unit = 0;  // 0=Flatbed, 3=DocumentFeeder.

  bool duplex = false;

  // Scan rectangle in ICAP_UNITS (offset + extent). Presence-aware per bound:
  // the area is honoured only when all four are present and positive.
  bool has_offset_x = false;
  int offset_x = 0;
  bool has_offset_y = false;
  int offset_y = 0;
  bool has_width = false;
  int width = 0;
  bool has_height = false;
  int height = 0;
};

// Turns the raw nested ICAP selection into a ScanRequest. Pure. Resolution
// prefers ICAP_XRESOLUTION and falls back to ICAP_YRESOLUTION; pixel type and
// functional unit map straight through; the offset+extent become the scan area
// (via CornersFromUserScanArea) only when the full positive rectangle is present
// AND the units are pixels (or unspecified). Missing selections stay absent so
// TranslateScanParams applies the defaults.
ScanRequest ScanRequestFromIcap(const IcapScanSelection& sel);

// Bounds the module advertises to the host, used to clamp the request so a
// translated Params can never ask the device for an impossible value. `max_dpi`
// is the largest resolution the offer table advertises (PLAN-2-DESIGN.md: clamp
// resolution to the ESC I offer maximum).
struct ScanLimits {
  int max_dpi = 600;
};

// Default resolution when the host supplies none or an invalid one.
inline constexpr int kDefaultDpi = 300;
// Default brightness / contrast (mid-scale) when the host supplies none.
inline constexpr int kDefaultBrightnessContrast = 50;

// Translates `req` into a brscan::Params for a normal host-initiated RunScan.
// button_flow stays false throughout (this is the driver flow, not the scan
// button). Defaults per PLAN-2-DESIGN.md: 300 dpi (clamped to limits.max_dpi),
// RGB -> ScanMode::kColor, flatbed, brightness/contrast 50, full area. Duplex is
// forced off for the flatbed. brightness/contrast are clamped to 0..100.
Params TranslateScanParams(const ScanRequest& req, const ScanLimits& limits);

}  // namespace brscan::ica
