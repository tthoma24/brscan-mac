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

#include "brscan/types.h"

namespace brscan::ica {

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
