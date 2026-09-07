// Tests for the host scan-request -> brscan::Params translation
// (ica-module/scan_translate.h).
//
// A pure, hermetic unit: no ICADevices, no Foundation, no device. It maps the
// host's selection (resolution, pixel type, functional unit, duplex,
// brightness, contrast, area) onto a brscan::Params per PLAN-2-DESIGN.md's
// "Scan-parameter mapping" table, including resolution clamping, the
// full-area default, and the button_flow == false invariant.

#include "scan_translate.h"

#include <cstddef>
#include <iterator>

#include <gtest/gtest.h>

#include "brscan/types.h"
#include "paper_size.h"  // Read-only cross-check of the centered ADF x0.

namespace brscan::ica {
namespace {

// A request with everything absent -> the design defaults.
TEST(TranslateScanParamsTest, EmptyRequestUsesDefaults) {
  const Params p = TranslateScanParams(ScanRequest{}, ScanLimits{});
  EXPECT_EQ(p.x_dpi, kDefaultDpi);
  EXPECT_EQ(p.y_dpi, kDefaultDpi);
  EXPECT_EQ(p.mode, ScanMode::kColor);
  EXPECT_EQ(p.source, Source::kFlatbed);
  EXPECT_FALSE(p.duplex);
  EXPECT_EQ(p.brightness, kDefaultBrightnessContrast);
  EXPECT_EQ(p.contrast, kDefaultBrightnessContrast);
  EXPECT_EQ(p.area.x0, 0);
  EXPECT_EQ(p.area.y0, 0);
  EXPECT_EQ(p.area.x1, 0);
  EXPECT_EQ(p.area.y1, 0);
  EXPECT_FALSE(p.button_flow);  // Host-initiated driver flow, never the button.
}

// Pixel type -> ScanMode for every advertised value.
TEST(TranslateScanParamsTest, PixelTypeMapsToMode) {
  auto mode_for = [](int pixel_type) {
    ScanRequest r;
    r.has_pixel_type = true;
    r.pixel_type = pixel_type;
    return TranslateScanParams(r, ScanLimits{}).mode;
  };
  EXPECT_EQ(mode_for(0), ScanMode::kBlackWhite);  // 1-bit BW.
  EXPECT_EQ(mode_for(1), ScanMode::kGray);        // 8-bit gray.
  EXPECT_EQ(mode_for(2), ScanMode::kColor);       // RGB.
}

// Functional unit -> source; duplex only for the feeder.
TEST(TranslateScanParamsTest, FunctionalUnitMapsToSource) {
  ScanRequest flatbed;
  flatbed.has_functional_unit = true;
  flatbed.functional_unit = 0;
  EXPECT_EQ(TranslateScanParams(flatbed, ScanLimits{}).source,
            Source::kFlatbed);

  ScanRequest feeder;
  feeder.has_functional_unit = true;
  feeder.functional_unit = 3;
  EXPECT_EQ(TranslateScanParams(feeder, ScanLimits{}).source, Source::kAdf);
}

TEST(TranslateScanParamsTest, DuplexForcedOffOnFlatbed) {
  ScanRequest r;
  r.has_functional_unit = true;
  r.functional_unit = 0;  // flatbed.
  r.duplex = true;        // meaningless on the flatbed.
  EXPECT_FALSE(TranslateScanParams(r, ScanLimits{}).duplex);
}

TEST(TranslateScanParamsTest, DuplexHonouredForFeeder) {
  ScanRequest r;
  r.has_functional_unit = true;
  r.functional_unit = 3;  // feeder.
  r.duplex = true;
  EXPECT_TRUE(TranslateScanParams(r, ScanLimits{}).duplex);
}

// Resolution is used as-is when within the offered maximum, applied to both
// axes.
TEST(TranslateScanParamsTest, ResolutionAppliedToBothAxes) {
  ScanRequest r;
  r.has_resolution = true;
  r.resolution = 400;
  const Params p = TranslateScanParams(r, ScanLimits{/*max_dpi=*/600});
  EXPECT_EQ(p.x_dpi, 400);
  EXPECT_EQ(p.y_dpi, 400);
}

// Resolution above the offered maximum is clamped down.
TEST(TranslateScanParamsTest, ResolutionClampedToMax) {
  ScanRequest r;
  r.has_resolution = true;
  r.resolution = 1200;  // beyond the offer.
  const Params p = TranslateScanParams(r, ScanLimits{/*max_dpi=*/600});
  EXPECT_EQ(p.x_dpi, 600);
  EXPECT_EQ(p.y_dpi, 600);
}

// A non-positive or absent resolution falls back to the default.
TEST(TranslateScanParamsTest, InvalidResolutionFallsBackToDefault) {
  ScanRequest zero;
  zero.has_resolution = true;
  zero.resolution = 0;
  EXPECT_EQ(TranslateScanParams(zero, ScanLimits{}).x_dpi, kDefaultDpi);

  ScanRequest missing;  // has_resolution == false.
  EXPECT_EQ(TranslateScanParams(missing, ScanLimits{}).x_dpi, kDefaultDpi);
}

// Brightness / contrast pass through when in range and clamp to 0..100.
TEST(TranslateScanParamsTest, BrightnessContrastClampedToRange) {
  ScanRequest r;
  r.has_brightness = true;
  r.brightness = 150;  // over.
  r.has_contrast = true;
  r.contrast = -20;  // under.
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.brightness, 100);
  EXPECT_EQ(p.contrast, 0);
}

TEST(TranslateScanParamsTest, BrightnessContrastInRangePassThrough) {
  ScanRequest r;
  r.has_brightness = true;
  r.brightness = 25;
  r.has_contrast = true;
  r.contrast = 75;
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.brightness, 25);
  EXPECT_EQ(p.contrast, 75);
}

// A positive explicit rectangle is passed through as the scan area.
TEST(TranslateScanParamsTest, ExplicitAreaPassedThrough) {
  ScanRequest r;
  r.has_area = true;
  r.area_x0 = 10;
  r.area_y0 = 20;
  r.area_x1 = 2550;
  r.area_y1 = 3300;
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.area.x0, 10);
  EXPECT_EQ(p.area.y0, 20);
  EXPECT_EQ(p.area.x1, 2550);
  EXPECT_EQ(p.area.y1, 3300);
}

// A degenerate / non-positive rectangle means "full area" ({0,0,0,0}).
TEST(TranslateScanParamsTest, DegenerateAreaMeansFull) {
  ScanRequest r;
  r.has_area = true;
  r.area_x0 = 100;
  r.area_y0 = 100;
  r.area_x1 = 100;  // zero width.
  r.area_y1 = 200;
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.area.x0, 0);
  EXPECT_EQ(p.area.y0, 0);
  EXPECT_EQ(p.area.x1, 0);
  EXPECT_EQ(p.area.y1, 0);
}

// A flatbed request keeps the host's 0-based rectangle unchanged (the flatbed
// corner-registers; only the ADF is re-centered). This guards the "flatbed
// path unchanged" invariant against the ADF-centering added in this task.
TEST(TranslateScanParamsTest, FlatbedAreaNotRecentered) {
  ScanRequest r;
  r.has_functional_unit = true;
  r.functional_unit = 0;  // Flatbed.
  r.has_area = true;
  r.area_x0 = 0;
  r.area_y0 = 0;
  r.area_x1 = 2550;  // Letter width @300, 0-based.
  r.area_y1 = 3300;
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.source, Source::kFlatbed);
  EXPECT_EQ(p.area.x0, 0);
  EXPECT_EQ(p.area.x1, 2550);
  EXPECT_EQ(p.area.y0, 0);
  EXPECT_EQ(p.area.y1, 3300);
}

// An ADF request re-centers the requested width horizontally in the sensor,
// preserving the width and leaving y0/y1 alone. Letter width 2512 @300 centers
// at x0 = (3472 - 2512) / 2 = 480 (vs the flatbed's 0), so the blank left margin
// and right-edge cutoff of the un-centered 0-based rectangle are gone.
TEST(TranslateScanParamsTest, AdfAreaIsCenteredInSensor) {
  ScanRequest r;
  r.has_functional_unit = true;
  r.functional_unit = 3;  // Document feeder.
  r.has_resolution = true;
  r.resolution = 300;
  r.has_area = true;
  r.area_x0 = 0;
  r.area_y0 = 0;
  r.area_x1 = 2512;  // Letter ADF width @300.
  r.area_y1 = 3253;
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.source, Source::kAdf);
  EXPECT_EQ(p.area.x0, 480);
  EXPECT_EQ(p.area.x1, 480 + 2512);  // Width preserved.
  EXPECT_EQ(p.area.y0, 0);           // Vertical bounds untouched.
  EXPECT_EQ(p.area.y1, 3253);
}

// A non-zero requested offset does not survive ADF centering: the window is
// re-anchored from the sensor center, not shifted by the host's offset.
TEST(TranslateScanParamsTest, AdfCenteringIgnoresRequestedOffset) {
  ScanRequest r;
  r.has_functional_unit = true;
  r.functional_unit = 3;
  r.has_resolution = true;
  r.resolution = 300;
  r.has_area = true;
  r.area_x0 = 999;  // Arbitrary host offset -- discarded by centering.
  r.area_y0 = 12;
  r.area_x1 = 999 + 2512;
  r.area_y1 = 12 + 3253;
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.area.x0, 480);
  EXPECT_EQ(p.area.x1, 480 + 2512);
  EXPECT_EQ(p.area.y0, 12);  // y offset preserved.
  EXPECT_EQ(p.area.y1, 12 + 3253);
}

// The full-area default ({0,0,0,0}) is never centered -- it means "full offered
// area", not an explicit zero-width window.
TEST(TranslateScanParamsTest, AdfFullAreaNotCentered) {
  ScanRequest r;
  r.has_functional_unit = true;
  r.functional_unit = 3;  // Feeder, but no explicit area.
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.source, Source::kAdf);
  EXPECT_EQ(p.area.x0, 0);
  EXPECT_EQ(p.area.x1, 0);
  EXPECT_EQ(p.area.y0, 0);
  EXPECT_EQ(p.area.y1, 0);
}

// ---------------------------------------------------------------------------
// ADF centering math (AdfSensorWidthAtDpi / CenteredAdfX0).
// ---------------------------------------------------------------------------

TEST(AdfCenteringTest, SensorWidthScalesWithDpi) {
  EXPECT_EQ(AdfSensorWidthAtDpi(300), kAdfSensorWidthAt300);  // 3472.
  EXPECT_EQ(AdfSensorWidthAtDpi(600), 6944);                  // 3472 * 2.
  EXPECT_EQ(AdfSensorWidthAtDpi(150), 1736);                  // 3472 / 2.
  EXPECT_EQ(AdfSensorWidthAtDpi(0), 0);                       // Guard.
}

TEST(AdfCenteringTest, CentersNarrowWindow) {
  // (3472 - 2512) / 2 = 480.
  EXPECT_EQ(CenteredAdfX0(3472, 2512), 480);
}

TEST(AdfCenteringTest, ClampsWhenWidthMeetsOrExceedsSensor) {
  EXPECT_EQ(CenteredAdfX0(3472, 3472), 0);  // Exactly fills the sensor.
  EXPECT_EQ(CenteredAdfX0(3472, 4000), 0);  // Wider than the sensor -> clamp.
}

// Cross-check the centering against daemon/paper_size.cpp's captured ADF areas:
// centering a page's requested width (captured x1 - x0) in the sensor lands
// within a few px of the captured (device-registered) x0 for every ADF token.
// This is the read-only ground-truth check the brief calls for.
TEST(AdfCenteringTest, MatchesCapturedAreaForPaper) {
  constexpr int kDpi = 300;
  constexpr int kTolerance = 3;  // Brother's per-dpi rounding drift.
  for (const char* token : {"LETTER", "LEGAL", "A4", "LEDGER", "A3"}) {
    const std::optional<brscan::Area> captured =
        brscan::scand::AreaForPaper(token, kDpi);
    ASSERT_TRUE(captured.has_value()) << token;
    const int requested_width = captured->x1 - captured->x0;
    const int centered_x0 =
        CenteredAdfX0(AdfSensorWidthAtDpi(kDpi), requested_width);
    EXPECT_NEAR(centered_x0, captured->x0, kTolerance)
        << token << " requested_width=" << requested_width;
  }
}

// ---------------------------------------------------------------------------
// Paper token -> ICScannerDocumentType (ICAP_SUPPORTEDSIZES members). The raw
// values are the NON-contiguous ImageCaptureCore enum, verified against the SDK
// header ICScannerFunctionalUnits.h.
TEST(DocumentTypeForPaperTokenTest, StandardTokensMapToSdkValues) {
  EXPECT_EQ(DocumentTypeForPaperToken("LETTER"), kDocumentTypeUSLetter);  // 3
  EXPECT_EQ(DocumentTypeForPaperToken("LEGAL"), kDocumentTypeUSLegal);    // 4
  EXPECT_EQ(DocumentTypeForPaperToken("A4"), kDocumentTypeA4);            // 1
  EXPECT_EQ(DocumentTypeForPaperToken("LEDGER"), kDocumentTypeUSLedger);  // 9
  EXPECT_EQ(DocumentTypeForPaperToken("A3"), kDocumentTypeA3);            // 11
  EXPECT_EQ(DocumentTypeForPaperToken("A5"), kDocumentTypeA5);            // 5
  EXPECT_EQ(DocumentTypeForPaperToken("EXECUTIVE"),
            kDocumentTypeUSExecutive);  // 10
}

// The distinct raw values must not collide (guards against a copy/paste slip in
// the non-contiguous mapping).
TEST(DocumentTypeForPaperTokenTest, StandardValuesAreDistinct) {
  const int values[] = {
      DocumentTypeForPaperToken("LETTER"), DocumentTypeForPaperToken("LEGAL"),
      DocumentTypeForPaperToken("A4"),     DocumentTypeForPaperToken("LEDGER"),
      DocumentTypeForPaperToken("A3"),     DocumentTypeForPaperToken("A5"),
      DocumentTypeForPaperToken("EXECUTIVE")};
  for (size_t i = 0; i < std::size(values); ++i) {
    for (size_t j = i + 1; j < std::size(values); ++j) {
      EXPECT_NE(values[i], values[j]) << "collision at " << i << "," << j;
    }
  }
}

// PHOTO (4x6) and BCARD (business card) map to the SDK's photo/card document
// types so they render as named flatbed sizes (verified against the SDK header:
// ICScannerDocumentType4R = 62, ICScannerDocumentTypeBusinessCard = 53). They
// stay flatbed-only in the capability advertisement -- both are under the 148 mm
// ADF minimum -- but that split lives in scan_parameters.mm (BuildUnit); the
// token mapping itself is size-agnostic.
TEST(DocumentTypeForPaperTokenTest, PhotoMapsTo4R) {
  EXPECT_EQ(DocumentTypeForPaperToken("PHOTO"), kDocumentType4R);  // 62
}

TEST(DocumentTypeForPaperTokenTest, BusinessCardMapsToBusinessCard) {
  EXPECT_EQ(DocumentTypeForPaperToken("BCARD"), kDocumentTypeBusinessCard);  // 53
}

TEST(DocumentTypeForPaperTokenTest, UnknownTokenIsNone) {
  EXPECT_EQ(DocumentTypeForPaperToken("B4"), kDocumentTypeNone);
  EXPECT_EQ(DocumentTypeForPaperToken("letter"), kDocumentTypeNone);  // case.
  EXPECT_EQ(DocumentTypeForPaperToken(""), kDocumentTypeNone);
}

// The JIS B5 / JIS B4 ICScannerDocumentType values the module adds to
// ICAP_SUPPORTEDSIZES to match the Brother driver's flatbed list. They carry no
// daemon/paper_size.cpp geometry (the ICA host supplies the scan rectangle per
// document type), so they are advertised as raw enum values, not via a paper
// token; this pins those values to the non-contiguous SDK enum. JIS B5 is the
// base ICScannerDocumentTypeB5 = 2 (documented "B5/JIS B5"); JIS B4 is
// ICScannerDocumentTypeJISB4 = 38 (ICScannerFunctionalUnits.h).
TEST(DocumentTypeSizeConstantsTest, JisValuesMatchSdkEnum) {
  EXPECT_EQ(kDocumentTypeJISB5, 2);
  EXPECT_EQ(kDocumentTypeJISB4, 38);
}

// The 4R (4x6 photo) and Business Card values match the non-contiguous SDK enum
// (ICScannerFunctionalUnits.h: ICScannerDocumentType4R = 62 in the photo region
// E=60/3R=61/4R=62/5R=63; ICScannerDocumentTypeBusinessCard = 53).
TEST(DocumentTypeSizeConstantsTest, PhotoAndCardValuesMatchSdkEnum) {
  EXPECT_EQ(kDocumentType4R, 62);
  EXPECT_EQ(kDocumentTypeBusinessCard, 53);
}

// The flatbed-only additions must not collide with the standard/JIS sizes or the
// platten default (a duplicate would drop a size from the dropdown).
TEST(DocumentTypeSizeConstantsTest, PhotoAndCardValuesDistinctFromExistingSizes) {
  const int values[] = {
      kDocumentTypeDefault,     kDocumentTypeA4,     kDocumentTypeUSLetter,
      kDocumentTypeUSLegal,     kDocumentTypeA5,     kDocumentTypeUSLedger,
      kDocumentTypeUSExecutive, kDocumentTypeA3,     kDocumentTypeJISB5,
      kDocumentTypeJISB4,       kDocumentType4R,     kDocumentTypeBusinessCard};
  for (size_t i = 0; i < std::size(values); ++i) {
    for (size_t j = i + 1; j < std::size(values); ++j) {
      EXPECT_NE(values[i], values[j]) << "collision at " << i << "," << j;
    }
  }
}

// ---------------------------------------------------------------------------
// Per-unit ICAP_SUPPORTEDSIZES membership. The capability advertisement lives in
// scan_parameters.mm (BuildUnit), which needs Foundation; these pure tests pin
// the INTENDED per-unit sets so a regression there is caught without a device.
// The ADF feeds 148-297 mm wide x 148-431.8 mm long (Brother spec), so the
// feeder carries the full document set A5-up (no platten Default, no 4R/Business
// Card -- both under 148 mm); the flatbed carries that set plus Default, 4R, and
// Business Card.

// Every document type the feeder advertises fits the ADF envelope.
TEST(SupportedSizesPerUnitTest, FeederSet) {
  const int feeder[] = {
      kDocumentTypeUSLetter,    kDocumentTypeUSLegal, kDocumentTypeA4,
      kDocumentTypeUSLedger,    kDocumentTypeA3,      kDocumentTypeA5,
      kDocumentTypeUSExecutive, kDocumentTypeJISB5,   kDocumentTypeJISB4};
  auto contains = [&](int v) {
    for (int x : feeder)
      if (x == v) return true;
    return false;
  };
  // A5, Executive, JIS B5, JIS B4 are present (the sizes this task adds).
  EXPECT_TRUE(contains(kDocumentTypeA5));
  EXPECT_TRUE(contains(kDocumentTypeUSExecutive));
  EXPECT_TRUE(contains(kDocumentTypeJISB5));
  EXPECT_TRUE(contains(kDocumentTypeJISB4));
  // The platten default and the sub-148 mm sizes are NOT on the feeder.
  EXPECT_FALSE(contains(kDocumentTypeDefault));
  EXPECT_FALSE(contains(kDocumentType4R));
  EXPECT_FALSE(contains(kDocumentTypeBusinessCard));
}

// The flatbed set is the feeder set plus the platten default, 4R, and Business
// Card.
TEST(SupportedSizesPerUnitTest, FlatbedSet) {
  const int flatbed[] = {
      kDocumentTypeDefault,     kDocumentTypeUSLetter, kDocumentTypeUSLegal,
      kDocumentTypeA4,          kDocumentTypeUSLedger, kDocumentTypeA3,
      kDocumentTypeA5,          kDocumentTypeUSExecutive, kDocumentType4R,
      kDocumentTypeBusinessCard, kDocumentTypeJISB5,   kDocumentTypeJISB4};
  auto contains = [&](int v) {
    for (int x : flatbed)
      if (x == v) return true;
    return false;
  };
  EXPECT_TRUE(contains(kDocumentTypeDefault));
  EXPECT_TRUE(contains(kDocumentType4R));
  EXPECT_TRUE(contains(kDocumentTypeBusinessCard));
  // The flatbed is a superset of the feeder's A5-up additions.
  EXPECT_TRUE(contains(kDocumentTypeA5));
  EXPECT_TRUE(contains(kDocumentTypeUSExecutive));
  EXPECT_TRUE(contains(kDocumentTypeJISB5));
  EXPECT_TRUE(contains(kDocumentTypeJISB4));
}

// The JIS additions must not collide with any of the standard token-mapped
// sizes or the platten default (a duplicate would drop a size from the dropdown).
TEST(DocumentTypeSizeConstantsTest, JisValuesDistinctFromExistingSizes) {
  const int values[] = {
      kDocumentTypeDefault,     kDocumentTypeA4,     kDocumentTypeUSLetter,
      kDocumentTypeUSLegal,     kDocumentTypeA5,     kDocumentTypeUSLedger,
      kDocumentTypeUSExecutive, kDocumentTypeA3,     kDocumentTypeJISB5,
      kDocumentTypeJISB4};
  for (size_t i = 0; i < std::size(values); ++i) {
    for (size_t j = i + 1; j < std::size(values); ++j) {
      EXPECT_NE(values[i], values[j]) << "collision at " << i << "," << j;
    }
  }
}

// ---------------------------------------------------------------------------
// Host userScanArea (offset + extent, pixels) -> corner-bounded Area.
TEST(CornersFromUserScanAreaTest, PositiveRectConverts) {
  Area a{};
  ASSERT_TRUE(CornersFromUserScanArea(10, 20, 2540, 3280, &a));
  EXPECT_EQ(a.x0, 10);
  EXPECT_EQ(a.y0, 20);
  EXPECT_EQ(a.x1, 2550);
  EXPECT_EQ(a.y1, 3300);
}

TEST(CornersFromUserScanAreaTest, ZeroOffsetConverts) {
  Area a{};
  ASSERT_TRUE(CornersFromUserScanArea(0, 0, 100, 200, &a));
  EXPECT_EQ(a.x0, 0);
  EXPECT_EQ(a.y0, 0);
  EXPECT_EQ(a.x1, 100);
  EXPECT_EQ(a.y1, 200);
}

TEST(CornersFromUserScanAreaTest, NonPositiveExtentRejected) {
  Area a{-1, -1, -1, -1};
  EXPECT_FALSE(CornersFromUserScanArea(0, 0, 0, 200, &a));
  EXPECT_FALSE(CornersFromUserScanArea(0, 0, 100, 0, &a));
  EXPECT_FALSE(CornersFromUserScanArea(0, 0, -5, 200, &a));
  // `out` untouched on rejection.
  EXPECT_EQ(a.x0, -1);
  EXPECT_EQ(a.y0, -1);
  EXPECT_EQ(a.x1, -1);
  EXPECT_EQ(a.y1, -1);
}

TEST(CornersFromUserScanAreaTest, NullOutRejected) {
  EXPECT_FALSE(CornersFromUserScanArea(0, 0, 100, 100, nullptr));
}

// ---------------------------------------------------------------------------
// Measurement-unit -> pixel conversion (PixelsFromMeasure).
TEST(PixelsFromMeasureTest, InchesScaleByDpi) {
  EXPECT_EQ(PixelsFromMeasure(8.5, kIcapUnitsInches, 300), 2550);
  EXPECT_EQ(PixelsFromMeasure(11.0, kIcapUnitsInches, 300), 3300);
  EXPECT_EQ(PixelsFromMeasure(1.0, kIcapUnitsInches, 600), 600);
}

TEST(PixelsFromMeasureTest, PixelsPassThrough) {
  EXPECT_EQ(PixelsFromMeasure(2550.0, kIcapUnitsPixels, 300), 2550);
  EXPECT_EQ(PixelsFromMeasure(0.0, kIcapUnitsPixels, 300), 0);
}

TEST(PixelsFromMeasureTest, CentimetersScaleByDpiOver254) {
  EXPECT_EQ(PixelsFromMeasure(2.54, kIcapUnitsCentimeters, 300), 300);
  EXPECT_EQ(PixelsFromMeasure(25.4, kIcapUnitsCentimeters, 300), 3000);
}

TEST(PixelsFromMeasureTest, RoundsToNearestPixel) {
  // 8.505 in * 300 = 2551.5 -> rounds to 2552 (half rounds away from zero).
  EXPECT_EQ(PixelsFromMeasure(8.505, kIcapUnitsInches, 300), 2552);
}

TEST(PixelsFromMeasureTest, UnsupportedUnitAndBadDpiAreInvalid) {
  EXPECT_EQ(PixelsFromMeasure(8.5, /*points=*/3, 300), kMeasureInvalid);
  EXPECT_EQ(PixelsFromMeasure(8.5, kIcapUnitsInches, 0), kMeasureInvalid);
  EXPECT_EQ(PixelsFromMeasure(8.5, kIcapUnitsInches, -300), kMeasureInvalid);
}

// The corner output feeds TranslateScanParams unchanged: a converted positive
// rect flows through as the scan area.
TEST(CornersFromUserScanAreaTest, FeedsTranslateScanParams) {
  Area a{};
  ASSERT_TRUE(CornersFromUserScanArea(5, 6, 200, 300, &a));
  ScanRequest r;
  r.has_area = true;
  r.area_x0 = a.x0;
  r.area_y0 = a.y0;
  r.area_x1 = a.x1;
  r.area_y1 = a.y1;
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.area.x0, 5);
  EXPECT_EQ(p.area.y0, 6);
  EXPECT_EQ(p.area.x1, 205);
  EXPECT_EQ(p.area.y1, 306);
}

// ---------------------------------------------------------------------------
// Nested ICAP selection (host's userScanArea entries) -> ScanRequest. Pure; the
// CoreFoundation traversal that fills IcapScanSelection lives in module_main.mm.
TEST(ScanRequestFromIcapTest, EmptySelectionIsAllAbsent) {
  const ScanRequest r = ScanRequestFromIcap(IcapScanSelection{});
  EXPECT_FALSE(r.has_resolution);
  EXPECT_FALSE(r.has_pixel_type);
  EXPECT_FALSE(r.has_functional_unit);
  EXPECT_FALSE(r.has_area);
  // Feeds the defaults end to end.
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.x_dpi, kDefaultDpi);
  EXPECT_EQ(p.mode, ScanMode::kColor);
  EXPECT_EQ(p.source, Source::kFlatbed);
}

TEST(ScanRequestFromIcapTest, ResolutionPixelTypeAndUnitMapThrough) {
  IcapScanSelection sel;
  sel.has_x_resolution = true;
  sel.x_resolution = 100;
  sel.has_pixel_type = true;
  sel.pixel_type = 2;  // RGB.
  const ScanRequest r = ScanRequestFromIcap(sel);
  ASSERT_TRUE(r.has_resolution);
  EXPECT_EQ(r.resolution, 100);
  ASSERT_TRUE(r.has_pixel_type);
  EXPECT_EQ(r.pixel_type, 2);
  // End to end: the host's 100 dpi + RGB is honoured (the Task-7 bug ignored it).
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.x_dpi, 100);
  EXPECT_EQ(p.y_dpi, 100);
  EXPECT_EQ(p.mode, ScanMode::kColor);
}

TEST(ScanRequestFromIcapTest, ResolutionFallsBackToYAxis) {
  IcapScanSelection sel;
  sel.has_y_resolution = true;
  sel.y_resolution = 200;  // X absent.
  const ScanRequest r = ScanRequestFromIcap(sel);
  ASSERT_TRUE(r.has_resolution);
  EXPECT_EQ(r.resolution, 200);
}

TEST(ScanRequestFromIcapTest, NonPositiveResolutionIsAbsent) {
  IcapScanSelection sel;
  sel.has_x_resolution = true;
  sel.x_resolution = 0;
  EXPECT_FALSE(ScanRequestFromIcap(sel).has_resolution);
}

TEST(ScanRequestFromIcapTest, PixelTypeGrayAndBwMapThrough) {
  auto mode_for = [](int pixel_type) {
    IcapScanSelection sel;
    sel.has_pixel_type = true;
    sel.pixel_type = pixel_type;
    return TranslateScanParams(ScanRequestFromIcap(sel), ScanLimits{}).mode;
  };
  EXPECT_EQ(mode_for(0), ScanMode::kBlackWhite);
  EXPECT_EQ(mode_for(1), ScanMode::kGray);
  EXPECT_EQ(mode_for(2), ScanMode::kColor);
}

TEST(ScanRequestFromIcapTest, FunctionalUnitAndDuplexMapThrough) {
  IcapScanSelection sel;
  sel.has_functional_unit = true;
  sel.functional_unit = 3;  // feeder.
  sel.duplex = true;
  const Params p = TranslateScanParams(ScanRequestFromIcap(sel), ScanLimits{});
  EXPECT_EQ(p.source, Source::kAdf);
  EXPECT_TRUE(p.duplex);
}

// ---------------------------------------------------------------------------
// Source-selection precedence (Task 19). A feeder scan carries CAP_FEEDERENABLED
// == 1 but no functional unit; without this it defaulted to funit=0 -> flatbed.

// CAP_FEEDERENABLED == 1 with no functional unit selects the feeder (adf).
TEST(ScanRequestFromIcapTest, FeederEnabledSelectsAdf) {
  IcapScanSelection sel;
  sel.has_feeder_enabled = true;
  sel.feeder_enabled = 1;  // no functional unit present.
  const ScanRequest r = ScanRequestFromIcap(sel);
  ASSERT_TRUE(r.has_functional_unit);
  EXPECT_EQ(r.functional_unit, 3);
  EXPECT_EQ(r.source_signal, SourceSignal::kFeederEnabled);
  EXPECT_EQ(TranslateScanParams(r, ScanLimits{}).source, Source::kAdf);
}

// CAP_FEEDERENABLED == 0 does not force the feeder; it falls through.
TEST(ScanRequestFromIcapTest, FeederDisabledFallsThrough) {
  IcapScanSelection sel;
  sel.has_feeder_enabled = true;
  sel.feeder_enabled = 0;
  const ScanRequest r = ScanRequestFromIcap(sel);
  EXPECT_FALSE(r.has_functional_unit);
  EXPECT_EQ(r.source_signal, SourceSignal::kNone);
  EXPECT_EQ(TranslateScanParams(r, ScanLimits{}).source, Source::kFlatbed);
}

// An explicit functional unit wins over CAP_FEEDERENABLED.
TEST(ScanRequestFromIcapTest, ExplicitUnitBeatsFeederEnabled) {
  IcapScanSelection sel;
  sel.has_functional_unit = true;
  sel.functional_unit = 0;  // explicit flatbed.
  sel.has_feeder_enabled = true;
  sel.feeder_enabled = 1;  // contradicts, but loses.
  const ScanRequest r = ScanRequestFromIcap(sel);
  EXPECT_EQ(r.functional_unit, 0);
  EXPECT_EQ(r.source_signal, SourceSignal::kExplicitUnit);
  EXPECT_EQ(TranslateScanParams(r, ScanLimits{}).source, Source::kFlatbed);
}

// With neither an explicit unit nor CAP_FEEDERENABLED, the tracked unit decides.
TEST(ScanRequestFromIcapTest, TrackedUnitIsTheFallback) {
  IcapScanSelection sel;
  sel.has_tracked_functional_unit = true;
  sel.tracked_functional_unit = 3;  // feeder tracked from an earlier switch.
  const ScanRequest r = ScanRequestFromIcap(sel);
  ASSERT_TRUE(r.has_functional_unit);
  EXPECT_EQ(r.functional_unit, 3);
  EXPECT_EQ(r.source_signal, SourceSignal::kTrackedUnit);
  EXPECT_EQ(TranslateScanParams(r, ScanLimits{}).source, Source::kAdf);
}

// CAP_FEEDERENABLED beats the tracked unit (a fresh feeder request overrides a
// previously tracked flatbed).
TEST(ScanRequestFromIcapTest, FeederEnabledBeatsTrackedUnit) {
  IcapScanSelection sel;
  sel.has_feeder_enabled = true;
  sel.feeder_enabled = 1;
  sel.has_tracked_functional_unit = true;
  sel.tracked_functional_unit = 0;  // flatbed tracked, but feeder wins.
  const ScanRequest r = ScanRequestFromIcap(sel);
  EXPECT_EQ(r.functional_unit, 3);
  EXPECT_EQ(r.source_signal, SourceSignal::kFeederEnabled);
}

// Nothing at all -> no unit, signal none, flatbed default.
TEST(ScanRequestFromIcapTest, NoSourceSignalMeansFlatbedDefault) {
  const ScanRequest r = ScanRequestFromIcap(IcapScanSelection{});
  EXPECT_FALSE(r.has_functional_unit);
  EXPECT_EQ(r.source_signal, SourceSignal::kNone);
  EXPECT_EQ(TranslateScanParams(r, ScanLimits{}).source, Source::kFlatbed);
}

// ---------------------------------------------------------------------------
// Duplex keys (Task 19). The exact key the host echoes for the 2-sided toggle is
// unobserved, so any of the legacy `duplex` bool, CAP_DUPLEX, or
// CAP_DUPLEXENABLED being non-zero maps to params.duplex (feeder only).

TEST(ScanRequestFromIcapTest, CapDuplexEnabledMapsToDuplex) {
  IcapScanSelection sel;
  sel.has_functional_unit = true;
  sel.functional_unit = 3;  // feeder.
  sel.has_cap_duplex_enabled = true;
  sel.cap_duplex_enabled = 1;
  EXPECT_TRUE(ScanRequestFromIcap(sel).duplex);
  EXPECT_TRUE(TranslateScanParams(ScanRequestFromIcap(sel), ScanLimits{}).duplex);
}

TEST(ScanRequestFromIcapTest, CapDuplexNonZeroMapsToDuplex) {
  IcapScanSelection sel;
  sel.has_functional_unit = true;
  sel.functional_unit = 3;  // feeder.
  sel.has_cap_duplex = true;
  sel.cap_duplex = 1;  // TWDX_1PASSDUPLEX.
  EXPECT_TRUE(ScanRequestFromIcap(sel).duplex);
  EXPECT_TRUE(TranslateScanParams(ScanRequestFromIcap(sel), ScanLimits{}).duplex);
}

TEST(ScanRequestFromIcapTest, CapDuplexZeroIsNotDuplex) {
  IcapScanSelection sel;
  sel.has_functional_unit = true;
  sel.functional_unit = 3;  // feeder.
  sel.has_cap_duplex = true;
  sel.cap_duplex = 0;  // TWDX_NONE.
  sel.has_cap_duplex_enabled = true;
  sel.cap_duplex_enabled = 0;
  EXPECT_FALSE(ScanRequestFromIcap(sel).duplex);
}

// Duplex via CAP_DUPLEXENABLED is still gated on the feeder downstream.
TEST(ScanRequestFromIcapTest, DuplexKeyIgnoredOnFlatbed) {
  IcapScanSelection sel;
  sel.has_functional_unit = true;
  sel.functional_unit = 0;  // flatbed.
  sel.has_cap_duplex_enabled = true;
  sel.cap_duplex_enabled = 1;
  const ScanRequest r = ScanRequestFromIcap(sel);
  EXPECT_TRUE(r.duplex);  // the key was read,
  EXPECT_FALSE(TranslateScanParams(r, ScanLimits{}).duplex);  // but flatbed gates.
}

TEST(ScanRequestFromIcapTest, FullAreaInPixelsIsHonoured) {
  IcapScanSelection sel;
  sel.has_units = true;
  sel.units = kIcapUnitsPixels;
  sel.has_offset_x = true;
  sel.offset_x = 10;
  sel.has_offset_y = true;
  sel.offset_y = 20;
  sel.has_width = true;
  sel.width = 500;
  sel.has_height = true;
  sel.height = 700;
  const ScanRequest r = ScanRequestFromIcap(sel);
  ASSERT_TRUE(r.has_area);
  EXPECT_EQ(r.area_x0, 10);
  EXPECT_EQ(r.area_y0, 20);
  EXPECT_EQ(r.area_x1, 510);
  EXPECT_EQ(r.area_y1, 720);
}

TEST(ScanRequestFromIcapTest, AreaHonouredWhenUnitsUnspecified) {
  IcapScanSelection sel;  // has_units == false.
  sel.has_offset_x = true;
  sel.has_offset_y = true;
  sel.has_width = true;
  sel.width = 100;
  sel.has_height = true;
  sel.height = 200;
  EXPECT_TRUE(ScanRequestFromIcap(sel).has_area);
}

// A US Letter selection in INCHES at 300 dpi converts to the canonical
// 2550x3300 pixel rectangle -- the geometry the final file scan must still
// produce after the platen is advertised in inches (Task 14).
TEST(ScanRequestFromIcapTest, LetterInchesAt300ConvertsTo2550x3300) {
  IcapScanSelection sel;
  sel.has_x_resolution = true;
  sel.x_resolution = 300;
  sel.has_units = true;
  sel.units = kIcapUnitsInches;
  sel.has_offset_x = true;
  sel.offset_x = 0.0;
  sel.has_offset_y = true;
  sel.offset_y = 0.0;
  sel.has_width = true;
  sel.width = 8.5;
  sel.has_height = true;
  sel.height = 11.0;
  const ScanRequest r = ScanRequestFromIcap(sel);
  ASSERT_TRUE(r.has_area);
  EXPECT_EQ(r.area_x0, 0);
  EXPECT_EQ(r.area_y0, 0);
  EXPECT_EQ(r.area_x1, 2550);
  EXPECT_EQ(r.area_y1, 3300);
}

// Inches convert at the request's dpi; with no resolution the default (300)
// applies, and a non-zero inch offset is converted too.
TEST(ScanRequestFromIcapTest, InchesUseDefaultDpiAndConvertOffset) {
  IcapScanSelection sel;  // no resolution -> kDefaultDpi (300).
  sel.has_units = true;
  sel.units = kIcapUnitsInches;
  sel.has_offset_x = true;
  sel.offset_x = 1.0;
  sel.has_offset_y = true;
  sel.offset_y = 0.5;
  sel.has_width = true;
  sel.width = 2.0;
  sel.has_height = true;
  sel.height = 3.0;
  const ScanRequest r = ScanRequestFromIcap(sel);
  ASSERT_TRUE(r.has_area);
  EXPECT_EQ(r.area_x0, 300);
  EXPECT_EQ(r.area_y0, 150);
  EXPECT_EQ(r.area_x1, 900);   // (1 + 2) in * 300.
  EXPECT_EQ(r.area_y1, 1050);  // (0.5 + 3) in * 300.
}

// Centimeters convert to pixels at dpi / 2.54.
TEST(ScanRequestFromIcapTest, CentimetersConvertAtDpiOver254) {
  IcapScanSelection sel;
  sel.has_x_resolution = true;
  sel.x_resolution = 300;
  sel.has_units = true;
  sel.units = kIcapUnitsCentimeters;
  sel.has_offset_x = true;
  sel.offset_x = 0.0;
  sel.has_offset_y = true;
  sel.offset_y = 0.0;
  sel.has_width = true;
  sel.width = 2.54;  // 1 inch -> 300 px.
  sel.has_height = true;
  sel.height = 5.08;  // 2 inches -> 600 px.
  const ScanRequest r = ScanRequestFromIcap(sel);
  ASSERT_TRUE(r.has_area);
  EXPECT_EQ(r.area_x1, 300);
  EXPECT_EQ(r.area_y1, 600);
}

// An unsupported unit (e.g. points = 3) leaves the area absent -> full area.
TEST(ScanRequestFromIcapTest, UnsupportedUnitRejectsArea) {
  IcapScanSelection sel;
  sel.has_units = true;
  sel.units = 3;  // points -- not offered by this module.
  sel.has_offset_x = true;
  sel.has_offset_y = true;
  sel.has_width = true;
  sel.width = 100;
  sel.has_height = true;
  sel.height = 200;
  EXPECT_FALSE(ScanRequestFromIcap(sel).has_area);
}

TEST(ScanRequestFromIcapTest, PartialAreaIsNotHonoured) {
  IcapScanSelection sel;
  sel.has_offset_x = true;
  sel.has_offset_y = true;
  sel.has_width = true;
  sel.width = 100;
  // height absent.
  EXPECT_FALSE(ScanRequestFromIcap(sel).has_area);
}

}  // namespace
}  // namespace brscan::ica
