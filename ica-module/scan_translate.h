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
// JIS B5 is the SDK's base ICScannerDocumentTypeB5 = 2, documented "B5/JIS B5,
// 182.00 mm x 257.00 mm" (ICScannerFunctionalUnits.h enum line 204, doc comment
// line 125). The JIS B-series enum (JISB4=38, JISB6=39, ...) deliberately skips
// B5 because this base value already denotes JIS B5, which is what the Brother
// driver lists.
inline constexpr int kDocumentTypeJISB5 = 2;
inline constexpr int kDocumentTypeUSLetter = 3;
inline constexpr int kDocumentTypeUSLegal = 4;
inline constexpr int kDocumentTypeA5 = 5;
inline constexpr int kDocumentTypeUSLedger = 9;
inline constexpr int kDocumentTypeUSExecutive = 10;
inline constexpr int kDocumentTypeA3 = 11;
// JIS B4 = ICScannerDocumentTypeJISB4 = 38, documented "JIS B4, 257.00 mm x
// 364.00 mm" (ICScannerFunctionalUnits.h enum line 239, doc comment line 160).
inline constexpr int kDocumentTypeJISB4 = 38;

// Maps a daemon/paper_size.cpp paper token to its ICScannerDocumentType value,
// or kDocumentTypeNone for tokens with no clean standard case (PHOTO, BCARD --
// exposed as a custom scan area only) and for unknown tokens. Pure; the single
// source of truth shared by the capability advertisement (scan_parameters.mm)
// and any reverse lookup, so the two never diverge.
int DocumentTypeForPaperToken(const std::string& token);

// Converts a host userScanArea (offset + extent, already in pixels) to the
// corner-bounded Area{x0,y0,x1,y1} brscan uses (x0=offX, y0=offY, x1=offX+width,
// y1=offY+height). Returns true and fills `out` only for a positive rectangle
// (width > 0 && height > 0); otherwise returns false and leaves `out` untouched
// so the caller falls back to the full-area default. Pure.
bool CornersFromUserScanArea(int offset_x, int offset_y, int width, int height,
                             Area* out);

// ICScannerMeasurementUnit raw values (SDK ICScannerFunctionalUnits.h), the same
// vocabulary the host echoes back in ICAP_UNITS: inches=0, centimeters=1,
// pixels=5. picas/points/twips (2/3/4) are defined by the SDK but not offered by
// this module, so they are treated as unsupported for the scan area.
inline constexpr int kIcapUnitsInches = 0;
inline constexpr int kIcapUnitsCentimeters = 1;

// A sentinel returned by PixelsFromMeasure for an unsupported unit.
inline constexpr int kMeasureInvalid = -2147483647;  // INT_MIN + 1.

// Converts one scan-area coordinate/extent expressed in the host measurement
// `unit` to pixels at `dpi`, rounding to the nearest pixel. The module advertises
// the platen's physical dimensions in inches (see PLAN-2-DESIGN.md and
// scan_parameters.mm) and the host echoes the scan rectangle back in whatever it
// has ICAP_UNITS set to, so the offset/extent it sends must be brought into the
// module's pixel space here. Conventions (confirmed against a working ICA scanner
// module's SetParameters unit handling -- facts only, no source copied):
//   - inches (0):      pixels = value * dpi
//   - centimeters (1): pixels = value * dpi / 2.54
//   - pixels (5):      pixels = value        (already pixels)
// Any other unit returns kMeasureInvalid so the caller falls back to the full
// offered area rather than scanning a mis-scaled rectangle. `dpi` must be > 0.
// Pure.
int PixelsFromMeasure(double value, int unit, int dpi);

// Which of the source-selection signals decided the scan source. The host can
// indicate the document feeder three different ways (an explicit functional
// unit, CAP_FEEDERENABLED == 1, or the unit tracked from an earlier unit-switch
// SetParameters); this records which one won so the decision can be logged.
enum class SourceSignal {
  kNone,          // No signal present -> flatbed default.
  kExplicitUnit,  // functionalUnit / selectedFunctionalUnitType in the request.
  kFeederEnabled,  // CAP_FEEDERENABLED == 1 in the request.
  kTrackedUnit,   // The DeviceContext.selectedFunctionalUnit fallback.
};

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

  // Which signal decided `functional_unit` (set by ScanRequestFromIcap). Purely
  // diagnostic: module_main.mm logs it so a feeder scan that still ran as flatbed
  // can be traced to the missing signal. Does not affect TranslateScanParams.
  SourceSignal source_signal = SourceSignal::kNone;

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

// TWAIN ICAP_UNITS raw value for pixels (TWUN_PIXELS / ICScannerMeasurementUnit
// pixels). The scan-area offset/extent are expressed in the host's ICAP_UNITS;
// inches (0), centimeters (1), and pixels (5) are converted to pixels by
// PixelsFromMeasure, and any other unit falls back to the full offered area.
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
  int units = 0;  // ICAP_UNITS.value; inches=0, cm=1, pixels=5.

  bool has_functional_unit = false;
  int functional_unit = 0;  // 0=Flatbed, 3=DocumentFeeder.

  // CAP_FEEDERENABLED as echoed in the scan request. When the user picks the
  // feeder and scans, the host sends CAP_FEEDERENABLED == 1 but (per the live
  // trace) NO functional unit, so this is the signal that a feeder scan is
  // wanted when `has_functional_unit` is false. 0 = disabled, 1 = enabled.
  bool has_feeder_enabled = false;
  int feeder_enabled = 0;

  // The functional unit the module tracked from an earlier unit-switch
  // SetParameters (DeviceContext.selectedFunctionalUnit). Used as the source
  // fallback when the current request carries neither an explicit unit nor
  // CAP_FEEDERENABLED. Absent (has_* false) when there is no device context.
  bool has_tracked_functional_unit = false;
  int tracked_functional_unit = 0;  // 0=Flatbed, 3=DocumentFeeder.

  bool duplex = false;  // Legacy plain `duplex` bool key, if the host sends it.

  // The TWAIN duplex capabilities the host may echo back for the feeder. The
  // exact key that carries the 2-sided toggle is not yet observed live (the
  // control did not render before this task advertised it), so ScanRequestFromIcap
  // treats any of these as "duplex on": CAP_DUPLEX (TWAIN duplexer type; 0=none,
  // 1=1-pass, 2=2-pass) and CAP_DUPLEXENABLED (the read/write enable toggle,
  // 0/1). module_main.mm logs the full request dict so the actual key can be
  // confirmed and this narrowed in a follow-up.
  bool has_cap_duplex = false;
  int cap_duplex = 0;
  bool has_cap_duplex_enabled = false;
  int cap_duplex_enabled = 0;

  // Scan rectangle in ICAP_UNITS (offset + extent), as doubles because a
  // non-pixel unit (inches) carries fractional values like 8.5. Presence-aware
  // per bound: the area is honoured only when all four are present, the unit is
  // convertible (inches/cm/pixels), and the resulting pixel rectangle is
  // positive. ScanRequestFromIcap converts these to pixels at the request's dpi.
  bool has_offset_x = false;
  double offset_x = 0.0;
  bool has_offset_y = false;
  double offset_y = 0.0;
  bool has_width = false;
  double width = 0.0;
  bool has_height = false;
  double height = 0.0;
};

// Turns the raw nested ICAP selection into a ScanRequest. Pure. Resolution
// prefers ICAP_XRESOLUTION and falls back to ICAP_YRESOLUTION; pixel type maps
// straight through. The scan SOURCE is resolved by precedence (recorded in
// ScanRequest.source_signal): an explicit functionalUnit /
// selectedFunctionalUnitType wins, else CAP_FEEDERENABLED == 1 selects the
// document feeder, else the tracked_functional_unit fallback, else absent (so
// TranslateScanParams defaults to the flatbed). This is the fix for a feeder
// scan that carried only CAP_FEEDERENABLED and therefore ran as flatbed. Duplex
// is set from any observed duplex key (the legacy `duplex` bool, CAP_DUPLEX != 0,
// or CAP_DUPLEXENABLED != 0) and is honoured only for the feeder downstream. The
// offset+extent become the scan area
// only when the full rectangle is present, the unit converts to pixels
// (inches/cm/pixels, via PixelsFromMeasure at the request's dpi -- default 300
// when the host sends no resolution; an unspecified unit is treated as pixels),
// and the converted pixel rectangle is positive (via CornersFromUserScanArea).
// Missing selections stay absent so TranslateScanParams applies the defaults.
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
