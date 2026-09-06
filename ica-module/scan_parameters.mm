// Plan 2 Task 8 — scanner capability enumeration for the ICA module.
// See scan_parameters.h for the contract and the clean-room note on keys.

#import "scan_parameters.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <optional>
#include <string>

#include "brscan/types.h"    // brscan::Area.
#include "paper_size.h"      // daemon/ — reused ground-truth paper geometry.
#include "scan_translate.h"  // DocumentTypeForPaperToken + the doc-type values.

namespace brscan::ica {

namespace {

// Reference resolution the advertised paper extents are reported at. ICAP_UNITS
// is advertised as pixels (below), and pixel extents depend on dpi, so the
// PHYSICALWIDTH/HEIGHT extents are captured at 300 dpi to match the ground-truth
// geometry table (daemon/paper_size.cpp) exactly.
constexpr int kReferenceDpi = 300;

// Resolutions the Brother MFC-J6920DW offers over the raw-scan protocol (device
// black-box facts; the scan-execution path clamps the chosen value to the live
// ESC I offer per PLAN-2-DESIGN.md). Advertised as ICAP_XRESOLUTION /
// ICAP_YRESOLUTION.
constexpr int kResolutions[] = {100, 150, 200, 300, 400, 600};
constexpr int kDefaultResolution = 300;

// ICScannerBitDepth values (SDK ICScannerFunctionalUnits.h): 1-bit for
// black-and-white, 8-bit otherwise. The raw value equals the bit count. The
// colour choice rides on ICAP_PIXELTYPE (advertised below) and ICAP_BITDEPTH.
constexpr int kBitDepth1 = 1;
constexpr int kBitDepth8 = 8;
constexpr int kDefaultBitDepth = 8;

// ICScannerPixelDataType values (SDK ICScannerFunctionalUnits.h, documented as
// "Corresponds to ICAP_PIXELTYPE of the TWAIN Specification"): BW=0, Gray=1,
// RGB=2. Advertised as ICAP_PIXELTYPE so Image Capture renders the colour-mode
// picker; RGB is the default. Pairs with the "scan mode" echo SetParameters maps
// back to a ScanMode (module_main.mm::PixelTypeForScanMode).
constexpr int kPixelTypeBw = 0;
constexpr int kPixelTypeGray = 1;
constexpr int kPixelTypeRgb = 2;
constexpr int kDefaultPixelType = kPixelTypeRgb;

// ICScannerMeasurementUnit value for pixels (SDK: inches=0, cm=1, picas=2,
// points=3, twips=4, pixels=5). Advertised as ICAP_UNITS so the host's scan-area
// geometry stays in the same pixel space the module already scans in.
constexpr int kMeasurementUnitPixels = 5;

// ICScannerFunctionalUnitType values (SDK): flatbed=0, documentFeeder=3.
constexpr int kFunctionalUnitFlatbed = 0;
constexpr int kFunctionalUnitFeeder = 3;

// The functional unit whose capabilities are advertised flat inside `device`
// (see BuildScannerParameters). The flatbed is the default selection.
constexpr int kSelectedFunctionalUnit = kFunctionalUnitFlatbed;

// TWAIN CAP_DUPLEX value for "no duplex" (TWDX_NONE). CAP_FEEDERENABLED /
// CAP_DUPLEX are advertised so the source and duplex controls appear; for the
// selected flatbed unit both are off.
constexpr int kDuplexNone = 0;
constexpr int kFeederDisabled = 0;

// TWAIN container value-type tags (TWON_*), the TWAIN Specification vocabulary
// that Image Capture's TWAIN-derived capability dictionaries use. INTERFACE
// FACTS; see the clean-room note in scan_parameters.h.
NSString* const kTwonEnumeration = @"TWON_ENUMERATION";
NSString* const kTwonOneValue = @"TWON_ONEVALUE";

// Keys of the TWAIN-style capability container. These four are our own
// descriptive names; the load-bearing facts are the ICAP_* capability keys and
// the TWON_* type tags they carry.
NSString* const kKeyType = @"type";
NSString* const kKeyValue = @"value";
NSString* const kKeyCurrent = @"current";
NSString* const kKeyDefault = @"default";

// The nine standard sizes we know geometry for, split by whether the document
// feeder can take them. daemon/paper_size.cpp documents that
// LETTER/LEGAL/A4/LEDGER/A3 feed through the ADF, while A5/EXECUTIVE/PHOTO/BCARD
// register on the flatbed only.
struct PaperChoice {
  const char* token;  // Exact daemon/paper_size.cpp token.
  bool flatbedOnly;
};
constexpr PaperChoice kPaperChoices[] = {
    {"LETTER", false}, {"LEGAL", false},    {"A4", false},
    {"LEDGER", false}, {"A3", false},       {"A5", true},
    {"EXECUTIVE", true}, {"PHOTO", true},   {"BCARD", true},
};

NSNumber* Int(int v) { return [NSNumber numberWithInt:v]; }

// A TWON_ENUMERATION capability: the full set of allowed values plus the current
// and default selections.
NSDictionary* Enumeration(NSArray* values, int current, int def) {
  return @{
    kKeyType : kTwonEnumeration,
    kKeyValue : values,
    kKeyCurrent : Int(current),
    kKeyDefault : Int(def),
  };
}

// A TWON_ONEVALUE capability: a single fixed scalar.
NSDictionary* OneValue(int value) {
  return @{
    kKeyType : kTwonOneValue,
    kKeyValue : Int(value),
    kKeyCurrent : Int(value),
    kKeyDefault : Int(value),
  };
}

NSArray* ResolutionArray() {
  NSMutableArray* r = [NSMutableArray array];
  for (int dpi : kResolutions) [r addObject:Int(dpi)];
  return r;
}

// Builds the ICAP_* capability set for one functional unit, flat (each key maps
// to a TWAIN-style container). Capabilities are scoped per unit because the
// flatbed and feeder differ: the flatbed exposes the platten
// (ICScannerDocumentTypeDefault) and the flatbed-only sizes and has the larger
// maximum extent, while the feeder omits the platten and the flatbed-only sizes.
// Only the selected unit's set is advertised this task (see
// BuildScannerParameters); the `feeder` parameter keeps the per-unit geometry
// correct and lets a follow-up switch the flat set when the host reselects.
NSDictionary* BuildUnit(bool feeder) {
  NSMutableArray* supportedSizes = [NSMutableArray array];
  // The platten "default" size is meaningful only for the flatbed.
  if (!feeder) [supportedSizes addObject:Int(kDocumentTypeDefault)];

  int maxWidthPx = 0;
  int maxHeightPx = 0;
  for (const PaperChoice& choice : kPaperChoices) {
    if (feeder && choice.flatbedOnly) continue;
    std::optional<brscan::Area> area =
        brscan::scand::AreaForPaper(choice.token, kReferenceDpi);
    if (area) {
      maxWidthPx = std::max(maxWidthPx, area->x1 - area->x0);
      maxHeightPx = std::max(maxHeightPx, area->y1 - area->y0);
    }
    const int docType = DocumentTypeForPaperToken(choice.token);
    if (docType != kDocumentTypeNone) [supportedSizes addObject:Int(docType)];
  }

  // Current/default supported size: the platten for the flatbed, else the first
  // feeder size (US Letter). Both are guaranteed present in supportedSizes.
  const int defaultSize =
      supportedSizes.count > 0
          ? [(NSNumber*)supportedSizes.firstObject intValue]
          : kDocumentTypeDefault;

  return @{
    @"ICAP_XRESOLUTION" :
        Enumeration(ResolutionArray(), kDefaultResolution, kDefaultResolution),
    @"ICAP_YRESOLUTION" :
        Enumeration(ResolutionArray(), kDefaultResolution, kDefaultResolution),
    @"ICAP_BITDEPTH" : Enumeration(@[ Int(kBitDepth1), Int(kBitDepth8) ],
                                   kDefaultBitDepth, kDefaultBitDepth),
    @"ICAP_PIXELTYPE" : Enumeration(
        @[ Int(kPixelTypeBw), Int(kPixelTypeGray), Int(kPixelTypeRgb) ],
        kDefaultPixelType, kDefaultPixelType),
    @"ICAP_PHYSICALWIDTH" : OneValue(maxWidthPx),
    @"ICAP_PHYSICALHEIGHT" : OneValue(maxHeightPx),
    @"ICAP_SUPPORTEDSIZES" :
        Enumeration(supportedSizes, defaultSize, defaultSize),
    @"ICAP_UNITS" : OneValue(kMeasurementUnitPixels),
  };
}

}  // namespace

void BuildScannerParameters(CFMutableDictionaryRef dict) {
  if (dict == nullptr) return;
  NSMutableDictionary* d = (__bridge NSMutableDictionary*)dict;

  // Everything the host reads lives under a single top-level `device` dict. The
  // ICAP_*/CAP_* capability entries for the SELECTED functional unit sit FLAT
  // inside it; a `functionalUnits` sub-dict then carries ONLY which unit types
  // exist and which is selected. (The previous shape put functionalUnits at the
  // top level with per-unit capability sub-dicts keyed "0"/"3"; Image Capture
  // rendered no controls from it. This flat-under-`device` layout is the shape
  // its scanner modules actually consume.)
  NSMutableDictionary* deviceDict =
      [NSMutableDictionary dictionaryWithDictionary:
                               BuildUnit(/*feeder=*/kSelectedFunctionalUnit ==
                                         kFunctionalUnitFeeder)];

  // Source (flatbed vs feeder) and duplex controls. TWON_ONEVALUE scalars for
  // the selected unit: the flatbed uses no feeder and no duplex.
  deviceDict[@"CAP_FEEDERENABLED"] = OneValue(kFeederDisabled);
  deviceDict[@"CAP_DUPLEX"] = OneValue(kDuplexNone);

  // Which functional units exist and which is selected -- nothing else.
  deviceDict[@"functionalUnits"] = @{
    @"availableFunctionalUnitTypes" :
        @[ Int(kFunctionalUnitFlatbed), Int(kFunctionalUnitFeeder) ],
    @"selectedFunctionalUnitType" : Int(kSelectedFunctionalUnit),
  };

  d[@"device"] = deviceDict;
}

}  // namespace brscan::ica
