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
// colour choice rides on ICAP_BITDEPTH plus the pixel type at scan time; we do
// NOT advertise a separate pixel-data-type list (no client mirror).
constexpr int kBitDepth1 = 1;
constexpr int kBitDepth8 = 8;
constexpr int kDefaultBitDepth = 8;

// ICScannerMeasurementUnit value for pixels (SDK: inches=0, cm=1, picas=2,
// points=3, twips=4, pixels=5). Advertised as ICAP_UNITS so the host's scan-area
// geometry stays in the same pixel space the module already scans in.
constexpr int kMeasurementUnitPixels = 5;

// ICScannerFunctionalUnitType values (SDK): flatbed=0, documentFeeder=3.
constexpr int kFunctionalUnitFlatbed = 0;
constexpr int kFunctionalUnitFeeder = 3;

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

// Builds the capability sub-dictionary for one functional unit. Capabilities are
// scoped per unit because the flatbed and feeder differ: the flatbed exposes the
// platten (ICScannerDocumentTypeDefault) and the flatbed-only sizes and has the
// larger maximum extent, while the feeder omits the platten and the flatbed-only
// sizes.
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

  // The capabilities are scoped per functional unit inside a top-level
  // functionalUnits dict. Alongside the available/selected type keys, each
  // unit's capability sub-dict is keyed by its ICScannerFunctionalUnitType value
  // rendered as a string ("0" flatbed, "3" feeder).
  NSMutableDictionary* functionalUnits = [NSMutableDictionary dictionary];
  functionalUnits[@"availableFunctionalUnitTypes"] =
      @[ Int(kFunctionalUnitFlatbed), Int(kFunctionalUnitFeeder) ];
  functionalUnits[@"selectedFunctionalUnitType"] = Int(kFunctionalUnitFlatbed);
  functionalUnits[[@(kFunctionalUnitFlatbed) stringValue]] =
      BuildUnit(/*feeder=*/false);
  functionalUnits[[@(kFunctionalUnitFeeder) stringValue]] =
      BuildUnit(/*feeder=*/true);

  d[@"functionalUnits"] = functionalUnits;
}

}  // namespace brscan::ica
