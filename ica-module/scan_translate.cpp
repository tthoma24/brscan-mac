// Plan 2 Task 7 — host scan-request -> brscan::Params translation.
// See scan_translate.h for the contract and the clean-room mapping note.

#include "scan_translate.h"

#include <algorithm>
#include <string>

#include "brscan/types.h"

namespace brscan::ica {

int DocumentTypeForPaperToken(const std::string& token) {
  // Tokens are the exact, upper-case daemon/paper_size.cpp names. PHOTO and
  // BCARD have no clean ICScannerDocumentType case, so they map to "none" and
  // are offered only as a custom scan area, per PLAN-2-DESIGN.md.
  if (token == "LETTER") return kDocumentTypeUSLetter;
  if (token == "LEGAL") return kDocumentTypeUSLegal;
  if (token == "A4") return kDocumentTypeA4;
  if (token == "LEDGER") return kDocumentTypeUSLedger;
  if (token == "A3") return kDocumentTypeA3;
  if (token == "A5") return kDocumentTypeA5;
  if (token == "EXECUTIVE") return kDocumentTypeUSExecutive;
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

namespace {

// ImageCaptureCore client enum values the module advertises and the host echoes.
constexpr int kPixelTypeBW = 0;
constexpr int kPixelTypeGray = 1;
constexpr int kPixelTypeRGB = 2;
constexpr int kFunctionalUnitFeeder = 3;  // else flatbed.

int Clamp(int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); }

}  // namespace

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
      req.has_functional_unit && req.functional_unit == kFunctionalUnitFeeder;
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
    p.area = Area{req.area_x0, req.area_y0, req.area_x1, req.area_y1};
  } else {
    p.area = Area{0, 0, 0, 0};  // Full offered area (RunScan honours this).
  }

  // Host-initiated driver flow: the normal ESC Q / RunScan path.
  p.button_flow = false;
  return p;
}

}  // namespace brscan::ica
