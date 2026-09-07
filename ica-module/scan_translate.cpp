// Plan 2 Task 7 — host scan-request -> brscan::Params translation.
// See scan_translate.h for the contract and the clean-room mapping note.

#include "scan_translate.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "brscan/types.h"

namespace brscan::ica {

namespace {
// ICScannerFunctionalUnitType raw value for the document feeder (SDK
// ICScannerFunctionalUnits.h); any other value is the flatbed. Shared by the
// source-precedence resolution in ScanRequestFromIcap and the source mapping in
// TranslateScanParams so the two never diverge.
constexpr int kFeederFunctionalUnit = 3;
}  // namespace

int DocumentTypeForPaperToken(const std::string& token) {
  // Tokens are the exact, upper-case daemon/paper_size.cpp names. PHOTO (4x6)
  // and BCARD (business card) map to the SDK's ICScannerDocumentType4R and
  // ICScannerDocumentTypeBusinessCard so they render as named flatbed sizes.
  if (token == "LETTER") return kDocumentTypeUSLetter;
  if (token == "LEGAL") return kDocumentTypeUSLegal;
  if (token == "A4") return kDocumentTypeA4;
  if (token == "LEDGER") return kDocumentTypeUSLedger;
  if (token == "A3") return kDocumentTypeA3;
  if (token == "A5") return kDocumentTypeA5;
  if (token == "EXECUTIVE") return kDocumentTypeUSExecutive;
  if (token == "PHOTO") return kDocumentType4R;
  if (token == "BCARD") return kDocumentTypeBusinessCard;
  return kDocumentTypeNone;
}

bool CornersFromUserScanArea(int offset_x, int offset_y, int width, int height,
                             Area* out) {
  if (out == nullptr || width <= 0 || height <= 0) return false;
  out->x0 = offset_x;
  out->y0 = offset_y;
  out->x1 = offset_x + width;
  out->y1 = offset_y + height;
  return true;
}

int PixelsFromMeasure(double value, int unit, int dpi) {
  if (dpi <= 0) return kMeasureInvalid;
  double pixels;
  switch (unit) {
    case kIcapUnitsInches:
      pixels = value * dpi;
      break;
    case kIcapUnitsCentimeters:
      pixels = value * dpi / 2.54;
      break;
    case kIcapUnitsPixels:
      pixels = value;
      break;
    default:
      return kMeasureInvalid;
  }
  return static_cast<int>(std::lround(pixels));
}

ScanRequest ScanRequestFromIcap(const IcapScanSelection& sel) {
  ScanRequest req;

  // Resolution: prefer X, fall back to Y (the host sends both, equal). A
  // non-positive value is treated as absent so the default (300) applies.
  if (sel.has_x_resolution && sel.x_resolution > 0) {
    req.has_resolution = true;
    req.resolution = sel.x_resolution;
  } else if (sel.has_y_resolution && sel.y_resolution > 0) {
    req.has_resolution = true;
    req.resolution = sel.y_resolution;
  }

  if (sel.has_pixel_type) {
    req.has_pixel_type = true;
    req.pixel_type = sel.pixel_type;
  }

  // Source precedence (Task 19). The host can signal the feeder three ways, so
  // resolve the source in priority order and record which signal won -- a feeder
  // scan that carried only CAP_FEEDERENABLED (no functional unit) previously
  // defaulted to funit=0 -> flatbed and hit the glass.
  //   1. an explicit functionalUnit / selectedFunctionalUnitType, else
  //   2. CAP_FEEDERENABLED == 1 -> the document feeder, else
  //   3. the tracked unit from an earlier unit-switch SetParameters, else
  //   4. absent -> TranslateScanParams defaults to the flatbed.
  if (sel.has_functional_unit) {
    req.has_functional_unit = true;
    req.functional_unit = sel.functional_unit;
    req.source_signal = SourceSignal::kExplicitUnit;
  } else if (sel.has_feeder_enabled && sel.feeder_enabled != 0) {
    req.has_functional_unit = true;
    req.functional_unit = kFeederFunctionalUnit;
    req.source_signal = SourceSignal::kFeederEnabled;
  } else if (sel.has_tracked_functional_unit) {
    req.has_functional_unit = true;
    req.functional_unit = sel.tracked_functional_unit;
    req.source_signal = SourceSignal::kTrackedUnit;
  }

  // Duplex: honour any observed duplex key. The exact key the host echoes for
  // the 2-sided toggle is not yet confirmed live, so the legacy `duplex` bool,
  // CAP_DUPLEX (!= 0), and CAP_DUPLEXENABLED (!= 0) are all treated as "on".
  // TranslateScanParams gates this on the feeder.
  req.duplex = sel.duplex ||
               (sel.has_cap_duplex_enabled && sel.cap_duplex_enabled != 0) ||
               (sel.has_cap_duplex && sel.cap_duplex != 0);

  // Scan area: honour the offset+extent only with a complete rectangle whose
  // unit converts to pixels. The host reports the rectangle in ICAP_UNITS; an
  // unspecified unit is treated as pixels (legacy). PixelsFromMeasure brings each
  // coordinate into the module's pixel space at the request's dpi (default 300
  // when the host sends no resolution), and CornersFromUserScanArea then rejects
  // a degenerate rect -- leaving has_area false (full offered area).
  const int area_dpi = req.has_resolution && req.resolution > 0
                           ? req.resolution
                           : kDefaultDpi;
  const int unit = sel.has_units ? sel.units : kIcapUnitsPixels;
  if (sel.has_offset_x && sel.has_offset_y && sel.has_width && sel.has_height) {
    const int px_off_x = PixelsFromMeasure(sel.offset_x, unit, area_dpi);
    const int px_off_y = PixelsFromMeasure(sel.offset_y, unit, area_dpi);
    const int px_width = PixelsFromMeasure(sel.width, unit, area_dpi);
    const int px_height = PixelsFromMeasure(sel.height, unit, area_dpi);
    Area corners{};
    if (px_off_x != kMeasureInvalid && px_off_y != kMeasureInvalid &&
        px_width != kMeasureInvalid && px_height != kMeasureInvalid &&
        CornersFromUserScanArea(px_off_x, px_off_y, px_width, px_height,
                                &corners)) {
      req.has_area = true;
      req.area_x0 = corners.x0;
      req.area_y0 = corners.y0;
      req.area_x1 = corners.x1;
      req.area_y1 = corners.y1;
    }
  }
  return req;
}

namespace {

// ImageCaptureCore client enum values the module advertises and the host echoes.
constexpr int kPixelTypeBW = 0;
constexpr int kPixelTypeGray = 1;
constexpr int kPixelTypeRGB = 2;

int Clamp(int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); }

// ADF full sensor width scaled to `dpi`, per PLAN-2-DESIGN.md's rounding
// contract (dpi/300, lround), matching daemon/paper_size.cpp.
constexpr int kAdfCaptureDpi = 300;

}  // namespace

int AdfSensorWidthAtDpi(int dpi) {
  if (dpi <= 0) return 0;
  return static_cast<int>(std::lround(
      kAdfSensorWidthAt300 * (static_cast<double>(dpi) / kAdfCaptureDpi)));
}

int CenteredAdfX0(int sensor_width_at_dpi, int requested_width) {
  // The window already spans (or overflows) the sensor -> corner-register.
  if (requested_width >= sensor_width_at_dpi) return 0;
  return std::max(0, (sensor_width_at_dpi - requested_width) / 2);
}

Params TranslateScanParams(const ScanRequest& req, const ScanLimits& limits) {
  Params p;  // brscan defaults: kColor, kFlatbed, 300 dpi, 50/50, full area.

  // Resolution: default 300, clamp to the offered maximum (never below 1).
  int dpi = (req.has_resolution && req.resolution > 0) ? req.resolution
                                                       : kDefaultDpi;
  const int max_dpi = limits.max_dpi > 0 ? limits.max_dpi : kDefaultDpi;
  dpi = Clamp(dpi, 1, max_dpi);
  p.x_dpi = dpi;
  p.y_dpi = dpi;

  // Colour / bit-depth ("pixel data type") -> ScanMode.
  if (req.has_pixel_type) {
    switch (req.pixel_type) {
      case kPixelTypeBW:
        p.mode = ScanMode::kBlackWhite;
        break;
      case kPixelTypeGray:
        p.mode = ScanMode::kGray;
        break;
      case kPixelTypeRGB:
      default:
        p.mode = ScanMode::kColor;
        break;
    }
  } else {
    p.mode = ScanMode::kColor;
  }

  // Functional unit -> source. Duplex is only meaningful for the feeder.
  const bool feeder =
      req.has_functional_unit && req.functional_unit == kFeederFunctionalUnit;
  p.source = feeder ? Source::kAdf : Source::kFlatbed;
  p.duplex = feeder && req.duplex;

  // Brightness / contrast, clamped to the 0..100 brscan scale (default 50).
  p.brightness = req.has_brightness
                     ? Clamp(req.brightness, 0, 100)
                     : kDefaultBrightnessContrast;
  p.contrast = req.has_contrast ? Clamp(req.contrast, 0, 100)
                                : kDefaultBrightnessContrast;

  // Scan area: an explicit positive rectangle in pixels, else full area.
  if (req.has_area && req.area_x1 > req.area_x0 && req.area_y1 > req.area_y0) {
    int x0 = req.area_x0;
    int x1 = req.area_x1;
    // The ADF center-registers pages within its sensor width, but the host
    // sends a 0-based rectangle. Re-center the requested width horizontally so
    // the page is framed correctly (no blank left margin / right-edge cutoff).
    // The flatbed corner-registers, so its rectangle is left exactly as sent.
    if (feeder) {
      const int width = x1 - x0;
      x0 = CenteredAdfX0(AdfSensorWidthAtDpi(dpi), width);
      x1 = x0 + width;
    }
    p.area = Area{x0, req.area_y0, x1, req.area_y1};
  } else {
    p.area = Area{0, 0, 0, 0};  // Full offered area (RunScan honours this).
  }

  // Host-initiated driver flow: the normal ESC Q / RunScan path.
  p.button_flow = false;
  return p;
}

}  // namespace brscan::ica
