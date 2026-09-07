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

// Reference dpi the ground-truth geometry table (daemon/paper_size.cpp) is
// captured at. The platen's physical extents are advertised in INCHES (see
// BuildUnit and PLAN-2-DESIGN.md), so the pixel extents read from the table at
// this dpi are divided by it to yield inches (pixels@300 / 300 = inches).
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

// ICScannerMeasurementUnit values (SDK ICScannerFunctionalUnits.h: inches=0,
// cm=1, picas=2, points=3, twips=4, pixels=5). Advertised as ICAP_UNITS. The
// unit defaults to INCHES: ICScannerFunctionalUnit.physicalSize is documented as
// "in current measurement unit", and a physical platen size has no meaning in
// pixels (pixels depend on dpi), so the host renders the overview platen from a
// real-world inch size. The host echoes the scan rectangle back in this unit and
// scan_translate::PixelsFromMeasure converts it to pixels. Inches/cm/pixels are
// all offered so the host may still request the rectangle in pixels.
constexpr int kMeasurementUnitInches = 0;
constexpr int kMeasurementUnitCentimeters = 1;
constexpr int kMeasurementUnitPixels = 5;

// ICScannerFunctionalUnitType values (SDK): flatbed=0, documentFeeder=3.
constexpr int kFunctionalUnitFlatbed = 0;
constexpr int kFunctionalUnitFeeder = 3;

// Source / duplex control values. CAP_FEEDERENABLED reflects the SELECTED unit
// (on when the feeder is selected, off for the flatbed) so the source picker
// tracks the choice. Duplex is a per-unit capability of the document feeder
// only: the SDK models it as ICScannerFunctionalUnitDocumentFeeder's
// `supportsDuplexScanning` (readonly BOOL) + `duplexScanningEnabled` (readwrite
// BOOL) -- ICScannerFunctionalUnits.h lines 802-811 -- which map onto the
// TWAIN-derived capability dict Image Capture consumes as CAP_DUPLEX (the
// duplexer type / "supported") and CAP_DUPLEXENABLED (the read/write on-off
// toggle).
//
// The 2-sided control renders only when these are advertised as TWON_ONEVALUE,
// not TWON_ENUMERATION: an enumeration [0, 1] with current 0 resolves to
// value[current] = 0 = no-duplex, so the framework sets supportsDuplexScanning =
// NO and draws no toggle. A live duplex-ADF module advertises them as ONEVALUE,
// so the FEEDER unit does the same: CAP_DUPLEX = OneValue(2) -- a NONZERO value
// is the load-bearing part (maps to supportsDuplexScanning = YES; 2 =
// TWDX_2PASSDUPLEX, as the reference uses; 1-vs-2 is immaterial to the flag) --
// and CAP_DUPLEXENABLED = OneValue(0), the read/write toggle the host flips to 1
// (maps to duplexScanningEnabled). The read side (scan_translate) already treats
// CAP_DUPLEXENABLED != 0 as 2-sided, so the host echoing it back on the scan
// request yields Params.duplex. CAP_AUTOFEED = OneValue(1) advertises the ADF's
// auto-feed, as the reference module does. The FLATBED has no duplexer, so it
// keeps a fixed CAP_DUPLEX = OneValue(0) (TWDX_NONE; no control) and no toggle.
constexpr int kDuplexNone = 0;         // TWDX_NONE (flatbed: fixed, no control).
constexpr int kDuplexTwoPass = 2;      // TWDX_2PASSDUPLEX (feeder: nonzero flag).
constexpr int kDuplexEnabledOff = 0;   // CAP_DUPLEXENABLED toggle, host-writable.
constexpr int kAutoFeedOn = 1;         // CAP_AUTOFEED (feeder auto-feed on).
constexpr int kFeederDisabled = 0;
constexpr int kFeederEnabled = 1;

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
// feeder can take them. The ADF feeds sheets 148-297 mm wide x 148-431.8 mm long
// (Brother spec), i.e. A5 up through A3/Ledger, so LETTER/LEGAL/A4/LEDGER/A3/A5/
// EXECUTIVE all feed through it. Only PHOTO (4x6) and BCARD (business card) are
// flatbed-only -- both are under the 148 mm ADF minimum.
struct PaperChoice {
  const char* token;  // Exact daemon/paper_size.cpp token.
  bool flatbedOnly;
};
constexpr PaperChoice kPaperChoices[] = {
    {"LETTER", false}, {"LEGAL", false},    {"A4", false},
    {"LEDGER", false}, {"A3", false},       {"A5", false},
    {"EXECUTIVE", false}, {"PHOTO", true},  {"BCARD", true},
};

NSNumber* Int(int v) { return [NSNumber numberWithInt:v]; }
NSNumber* Dbl(double v) { return [NSNumber numberWithDouble:v]; }

// A TWON_ONEVALUE capability carrying a real-valued scalar (e.g. a physical
// dimension in inches). The host reads floating-point physical extents as
// doubles.
NSDictionary* OneValueDouble(double value) {
  return @{
    kKeyType : kTwonOneValue,
    kKeyValue : Dbl(value),
    kKeyCurrent : Dbl(value),
    kKeyDefault : Dbl(value),
  };
}

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
// (ICScannerDocumentTypeDefault) plus the flatbed-only sizes (A6, 3R, 5R,
// business card, 4x6 photo) and has the larger maximum extent, while the feeder
// omits the flatbed-only sizes and reads Default as Auto / mixed-size.
// Only the selected unit's set is advertised this task (see
// BuildScannerParameters); the `feeder` parameter keeps the per-unit geometry
// correct and lets a follow-up switch the flat set when the host reselects.
NSDictionary* BuildUnit(bool feeder) {
  NSMutableArray* supportedSizes = [NSMutableArray array];
  // ICScannerDocumentTypeDefault (0) heads both units: it is the platten size for
  // the flatbed and the Auto / mixed-size choice for the feeder (the ADF has no
  // fixed page size, so the host lets the device auto-detect the sheet). Listing
  // it first makes it the current/default supported size for each unit.
  [supportedSizes addObject:Int(kDocumentTypeDefault)];

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

  // JIS B5 and JIS B4 round out the Brother driver's document size list (A4, JIS
  // B5, US Letter, US Legal, A5, US Ledger, US Executive, A3, JIS B4). They have
  // no daemon/paper_size.cpp geometry tuple and none is needed: for ICA the host
  // supplies the scan rectangle (userScanArea) for the selected document type,
  // so advertising the ICScannerDocumentType value alone makes the size
  // selectable and scannable. Both fit inside the advertised platen extent (JIS
  // B4 is 257x364 mm, smaller than A3/Ledger), so the physical bounds computed
  // above from the paper tokens do not regress. Advertised on BOTH units: JIS B4
  // (257x364 mm) and JIS B5 (182x257 mm) sit inside the ADF envelope, so the
  // feeder lists them too, matching the Brother driver.
  [supportedSizes addObject:Int(kDocumentTypeJISB5)];
  [supportedSizes addObject:Int(kDocumentTypeJISB4)];

  // A6 (105x148 mm) and the small photo sizes 3R (3.5x5) and 5R (5x7) are
  // FLATBED-ONLY: each is under the 148 mm ADF minimum width, so the feeder
  // cannot take them (the ADF is plain-paper only). Like JIS B4/B5 they carry no
  // daemon/paper_size.cpp geometry (the host supplies the scan rectangle per
  // document type), so their ICScannerDocumentType values are advertised
  // directly. All three fit well inside the advertised platen extent, so the
  // physical bounds computed above do not regress.
  if (!feeder) {
    [supportedSizes addObject:Int(kDocumentTypeA6)];
    [supportedSizes addObject:Int(kDocumentType3R)];
    [supportedSizes addObject:Int(kDocumentType5R)];
  }

  // Current/default supported size: the first entry, which is the platten Default
  // for the flatbed and the Auto (Default) choice for the feeder. Guaranteed
  // present (Default is added to both units above).
  const int defaultSize =
      supportedSizes.count > 0
          ? [(NSNumber*)supportedSizes.firstObject intValue]
          : kDocumentTypeDefault;

  // Physical platen extent in INCHES (ICAP_UNITS = inches, below). This is the
  // real scannable rectangle: its width is the widest sheet (A3, 3472 px@300 =
  // 11.57 in) and its height is the longest sheet (Ledger, 5053 px@300 = 16.84
  // in). Both A3 (3472x4913) and Ledger (3264x5053) fit inside it, so neither
  // regresses. It is NOT a phantom -- the J6920DW glass is A3-capable and the
  // bounding rectangle is what a real overview platen occupies; the host draws
  // the dashed platen and a paper-size selection at the correct proportion of it
  // (e.g. US Letter 8.5x11 in renders ~73% wide x ~65% tall). Expressed in
  // inches, not pixel counts, because a physical size in pixels is meaningless
  // (pixels depend on dpi) and the host mis-scaled the platen when it was given
  // pixel counts (Task 14 live defect).
  const double physWidthInches =
      static_cast<double>(maxWidthPx) / kReferenceDpi;
  const double physHeightInches =
      static_cast<double>(maxHeightPx) / kReferenceDpi;

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
    @"ICAP_PHYSICALWIDTH" : OneValueDouble(physWidthInches),
    @"ICAP_PHYSICALHEIGHT" : OneValueDouble(physHeightInches),
    @"ICAP_SUPPORTEDSIZES" :
        Enumeration(supportedSizes, defaultSize, defaultSize),
    // Offer inches (default), centimeters, and pixels; the host renders the
    // platen from the inch physical extents and may request the scan rectangle
    // in any of these (scan_translate::PixelsFromMeasure converts it to pixels).
    @"ICAP_UNITS" : Enumeration(@[ Int(kMeasurementUnitInches),
                                   Int(kMeasurementUnitCentimeters),
                                   Int(kMeasurementUnitPixels) ],
                                kMeasurementUnitInches, kMeasurementUnitInches),
  };
}

}  // namespace

void BuildScannerParameters(CFMutableDictionaryRef dict,
                            int selectedFunctionalUnitType) {
  if (dict == nullptr) return;
  NSMutableDictionary* d = (__bridge NSMutableDictionary*)dict;

  // The selected unit drives everything below. The host selects the feeder by
  // sending SetParameters with selectedFunctionalUnitType=3 and then re-calling
  // GetParameters; if we always answered with the flatbed the host would keep
  // re-selecting the feeder in a loop ("Waiting for Scanner"). Honour the
  // tracked selection: advertise that unit's caps flat and echo it back below.
  // Only the document feeder is treated as the feeder; any other value (default
  // 0) is the flatbed.
  const bool feeder = selectedFunctionalUnitType == kFunctionalUnitFeeder;

  // Everything the host reads lives under a single top-level `device` dict. The
  // ICAP_*/CAP_* capability entries for the SELECTED functional unit sit FLAT
  // inside it; a `functionalUnits` sub-dict then carries ONLY which unit types
  // exist and which is selected. (The previous shape put functionalUnits at the
  // top level with per-unit capability sub-dicts keyed "0"/"3"; Image Capture
  // rendered no controls from it. This flat-under-`device` layout is the shape
  // its scanner modules actually consume.)
  NSMutableDictionary* deviceDict =
      [NSMutableDictionary dictionaryWithDictionary:BuildUnit(feeder)];

  // Source (flatbed vs feeder) and duplex controls for the selected unit.
  // CAP_FEEDERENABLED tracks the selection. Duplex is a feeder-only capability:
  // the feeder advertises CAP_DUPLEX / CAP_DUPLEXENABLED as TWON_ONEVALUE (a
  // nonzero CAP_DUPLEX -> supportsDuplexScanning = YES, so Image Capture renders
  // a 2-sided toggle; CAP_DUPLEXENABLED = 0 is the read/write toggle the host
  // flips to 1) plus CAP_AUTOFEED. The flatbed has no duplexer, so it advertises
  // a fixed CAP_DUPLEX = TWDX_NONE (no control). See the constants note above.
  deviceDict[@"CAP_FEEDERENABLED"] =
      OneValue(feeder ? kFeederEnabled : kFeederDisabled);
  if (feeder) {
    deviceDict[@"CAP_DUPLEX"] = OneValue(kDuplexTwoPass);
    deviceDict[@"CAP_DUPLEXENABLED"] = OneValue(kDuplexEnabledOff);
    deviceDict[@"CAP_AUTOFEED"] = OneValue(kAutoFeedOn);
  } else {
    deviceDict[@"CAP_DUPLEX"] = OneValue(kDuplexNone);
  }

  // Which functional units exist and which is selected -- nothing else. The
  // selected type echoes the tracked value so the host's choice sticks.
  deviceDict[@"functionalUnits"] = @{
    @"availableFunctionalUnitTypes" :
        @[ Int(kFunctionalUnitFlatbed), Int(kFunctionalUnitFeeder) ],
    @"selectedFunctionalUnitType" :
        Int(feeder ? kFunctionalUnitFeeder : kFunctionalUnitFlatbed),
  };

  d[@"device"] = deviceDict;
}

}  // namespace brscan::ica
