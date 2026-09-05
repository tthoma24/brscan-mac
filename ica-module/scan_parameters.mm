// Plan 2 Task 2 — scanner parameter enumeration for the ICA module.
// See scan_parameters.h for the contract and the clean-room note on keys.

#import "scan_parameters.h"

#import <Foundation/Foundation.h>

#include <optional>
#include <string>
#include <vector>

#include "paper_size.h"     // daemon/ — reused ground-truth paper geometry.
#include "brscan/types.h"   // brscan::Area.

namespace brscan::ica {

namespace {

// Reference resolution the advertised paper extents are reported at. The host
// scales its own picker; the geometry table is captured at 300 dpi
// (daemon/paper_size.cpp), so report the extents there to avoid rounding.
constexpr int kReferenceDpi = 300;

// Resolutions the Brother MFC-J6920DW offers over the raw-scan protocol. These
// are device black-box facts (the ESC I offer), advertised statically here so
// the panel can show a picker without a live query; the scan-execution task
// will clamp the chosen value to the live ESC I offer per PLAN-2-DESIGN.md.
constexpr int kResolutions[] = {100, 150, 200, 300, 400, 600};

// Client-side enum values (ImageCaptureCore, public headers):
//   ICScannerPixelDataTypeBW=0, Gray=1, RGB=2
//   ICScannerBitDepth1Bit=1, 8Bits=8
//   ICScannerFunctionalUnitTypeFlatbed=0, DocumentFeeder=3
constexpr int kPixelTypeBW = 0;
constexpr int kPixelTypeGray = 1;
constexpr int kPixelTypeRGB = 2;
constexpr int kFunctionalUnitFlatbed = 0;
constexpr int kFunctionalUnitFeeder = 3;

// A standard paper size we expose, keyed by the daemon table's P= token.
struct PaperChoice {
  const char* token;       // exact daemon/paper_size.cpp token.
  const char* displayName;
  bool flatbedOnly;        // corner-registered flatbed sizes vs feeder sizes.
};

// The nine captured sizes, split flatbed-only vs feeder-capable exactly as
// daemon/paper_size.cpp documents (LETTER/LEGAL/A4/LEDGER/A3 center-register in
// the ADF; A5/EXECUTIVE/PHOTO/BCARD corner-register on the flatbed only).
constexpr PaperChoice kPaperChoices[] = {
    {"LETTER", "US Letter", false},
    {"LEGAL", "US Legal", false},
    {"A4", "A4", false},
    {"LEDGER", "US Ledger", false},
    {"A3", "A3", false},
    {"A5", "A5", true},
    {"EXECUTIVE", "Executive", true},
    {"PHOTO", "4x6 Photo", true},
    {"BCARD", "Business Card", true},
};

NSNumber* Int(int v) { return [NSNumber numberWithInt:v]; }

// Builds the array of standard paper sizes, each a dict of name + max scannable
// pixel extent at kReferenceDpi + physical size in points (1/72") + a
// flatbed-only flag. Geometry comes straight from brscan::scand::AreaForPaper.
NSArray* PaperSizeArray() {
  NSMutableArray* sizes = [NSMutableArray array];
  for (const PaperChoice& choice : kPaperChoices) {
    std::optional<brscan::Area> area =
        brscan::scand::AreaForPaper(choice.token, kReferenceDpi);
    if (!area) continue;  // Should not happen: tokens are from the same table.
    const int widthPx = area->x1 - area->x0;
    const int heightPx = area->y1 - area->y0;
    // Points at 1/72" = pixels / dpi * 72.
    const int widthPt = (widthPx * 72) / kReferenceDpi;
    const int heightPt = (heightPx * 72) / kReferenceDpi;
    [sizes addObject:@{
      @"name" : [NSString stringWithUTF8String:choice.displayName],
      @"token" : [NSString stringWithUTF8String:choice.token],
      @"widthPixels" : Int(widthPx),
      @"heightPixels" : Int(heightPx),
      @"widthPoints" : Int(widthPt),
      @"heightPoints" : Int(heightPt),
      @"referenceDPI" : Int(kReferenceDpi),
      @"flatbedOnly" : @(choice.flatbedOnly),
    }];
  }
  return sizes;
}

NSArray* ResolutionArray() {
  NSMutableArray* r = [NSMutableArray array];
  for (int dpi : kResolutions) [r addObject:Int(dpi)];
  return r;
}

}  // namespace

void BuildScannerParameters(CFMutableDictionaryRef dict) {
  if (dict == nullptr) return;
  NSMutableDictionary* d = (__bridge NSMutableDictionary*)dict;

  // Resolutions.
  d[@"supportedResolutions"] = ResolutionArray();
  d[@"resolution"] = Int(300);  // default.

  // Colour / pixel-data types (values are ICScannerPixelDataType).
  d[@"supportedPixelDataTypes"] =
      @[ Int(kPixelTypeBW), Int(kPixelTypeGray), Int(kPixelTypeRGB) ];
  d[@"pixelDataType"] = Int(kPixelTypeRGB);  // default: colour.

  // Bit depths (values are ICScannerBitDepth): 1-bit for BW, 8-bit otherwise.
  d[@"supportedBitDepths"] = @[ Int(1), Int(8) ];
  d[@"bitDepth"] = Int(8);

  // Functional units / sources (values are ICScannerFunctionalUnitType).
  d[@"supportedFunctionalUnits"] =
      @[ Int(kFunctionalUnitFlatbed), Int(kFunctionalUnitFeeder) ];
  d[@"functionalUnit"] = Int(kFunctionalUnitFlatbed);  // default: flatbed.
  d[@"supportsDuplex"] = @YES;  // feeder duplex; only meaningful for the feeder.

  // Brightness / contrast ranges (brscan::Params uses 0..100, default 50).
  d[@"brightness"] = Int(50);
  d[@"contrast"] = Int(50);
  d[@"brightnessRange"] = @{ @"min" : Int(0), @"max" : Int(100) };
  d[@"contrastRange"] = @{ @"min" : Int(0), @"max" : Int(100) };

  // Standard paper sizes within the device maximum.
  d[@"paperSizes"] = PaperSizeArray();
}

}  // namespace brscan::ica
