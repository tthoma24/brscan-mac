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
  EXPECT_EQ(mode_for(1), ScanMode::kTrueGray);    // 8-bit gray -> GRAY256/RLENGTH.
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
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.x_dpi, 400);
  EXPECT_EQ(p.y_dpi, 400);
}

// The default limits are the two sources' optical maxima (Brother spec): the
// flatbed reaches 2400 dpi, the ADF 1200 dpi.
TEST(TranslateScanParamsTest, DefaultLimitsMatchSourceOpticalMaxes) {
  EXPECT_EQ(kMaxFlatbedDpi, 2400);
  EXPECT_EQ(kMaxFeederDpi, 1200);
  EXPECT_EQ(ScanLimits{}.max_dpi_flatbed, kMaxFlatbedDpi);
  EXPECT_EQ(ScanLimits{}.max_dpi_feeder, kMaxFeederDpi);
}

// The flatbed advertises and honours up to its 2400 dpi optical maximum.
TEST(TranslateScanParamsTest, FlatbedAllowsUpTo2400) {
  ScanRequest r;
  r.has_resolution = true;
  r.resolution = 2400;
  r.has_functional_unit = true;
  r.functional_unit = 0;  // Flatbed.
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.x_dpi, 2400);
  EXPECT_EQ(p.y_dpi, 2400);
}

// With no functional unit the flatbed is the default source, so 2400 stands.
TEST(TranslateScanParamsTest, FlatbedIsDefaultSourceForResolutionCap) {
  ScanRequest r;
  r.has_resolution = true;
  r.resolution = 2400;  // No functional_unit -> flatbed cap (2400) applies.
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.x_dpi, 2400);
  EXPECT_EQ(p.y_dpi, 2400);
}

// A 2400 dpi request on the ADF is clamped to the feeder's 1200 dpi optical max.
TEST(TranslateScanParamsTest, FeederClampsResolutionTo1200) {
  ScanRequest r;
  r.has_resolution = true;
  r.resolution = 2400;  // Beyond the ADF's 1200 dpi optical max.
  r.has_functional_unit = true;
  r.functional_unit = 3;  // Document feeder.
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.x_dpi, 1200);
  EXPECT_EQ(p.y_dpi, 1200);
}

// 1200 dpi is valid on either source.
TEST(TranslateScanParamsTest, Resolution1200AllowedOnBothSources) {
  ScanRequest flatbed;
  flatbed.has_resolution = true;
  flatbed.resolution = 1200;
  flatbed.has_functional_unit = true;
  flatbed.functional_unit = 0;
  EXPECT_EQ(TranslateScanParams(flatbed, ScanLimits{}).x_dpi, 1200);

  ScanRequest feeder;
  feeder.has_resolution = true;
  feeder.resolution = 1200;
  feeder.has_functional_unit = true;
  feeder.functional_unit = 3;
  EXPECT_EQ(TranslateScanParams(feeder, ScanLimits{}).x_dpi, 1200);
}

// An over-max request clamps to the SELECTED source's maximum, not a flat cap.
TEST(TranslateScanParamsTest, OverMaxResolutionClampsToSourceMax) {
  ScanRequest flatbed;
  flatbed.has_resolution = true;
  flatbed.resolution = 9600;  // Absurdly high.
  flatbed.has_functional_unit = true;
  flatbed.functional_unit = 0;
  EXPECT_EQ(TranslateScanParams(flatbed, ScanLimits{}).x_dpi, 2400);

  ScanRequest feeder;
  feeder.has_resolution = true;
  feeder.resolution = 9600;
  feeder.has_functional_unit = true;
  feeder.functional_unit = 3;
  EXPECT_EQ(TranslateScanParams(feeder, ScanLimits{}).x_dpi, 1200);
}

// Custom per-source caps are honoured (a lower ceiling clamps the request).
TEST(TranslateScanParamsTest, CustomSourceCapsAreHonoured) {
  ScanRequest flatbed;
  flatbed.has_resolution = true;
  flatbed.resolution = 1200;
  flatbed.has_functional_unit = true;
  flatbed.functional_unit = 0;
  EXPECT_EQ(
      TranslateScanParams(flatbed, ScanLimits{/*flatbed=*/600, /*feeder=*/600})
          .x_dpi,
      600);
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

// A positive explicit rectangle is honoured as the scan area, with its width
// aligned DOWN to a multiple of 16 (the JPEG-MCU right-edge fringe fix; see the
// ScanWidthAlignTest block below). The offset (x0) and the vertical bounds pass
// through untouched -- only the width is aligned: width 2540 -> 2528, so
// x1 = 10 + 2528 = 2538.
TEST(TranslateScanParamsTest, ExplicitAreaHonouredWidthAlignedTo16) {
  ScanRequest r;
  r.has_area = true;
  r.area_x0 = 10;
  r.area_y0 = 20;
  r.area_x1 = 2550;
  r.area_y1 = 3300;
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.area.x0, 10);  // Offset untouched.
  EXPECT_EQ(p.area.y0, 20);
  EXPECT_EQ(p.area.x1, 2538);  // 10 + (2540 aligned down to 2528).
  EXPECT_EQ(p.area.y1, 3300);
  EXPECT_EQ((p.area.x1 - p.area.x0) % 16, 0);
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

// A flatbed request keeps the host's 0-based x0 (the flatbed corner-registers;
// only the ADF is re-centered), but its width is still aligned DOWN to a multiple
// of 16. This is exactly the US-Letter @300 fringe case from the bug report:
// 2550 px (159*16 + 6) -> 2544 (159*16). This guards both the "flatbed x0
// unchanged" invariant and the width alignment.
TEST(TranslateScanParamsTest, FlatbedAreaNotRecenteredWidthAlignedTo16) {
  ScanRequest r;
  r.has_functional_unit = true;
  r.functional_unit = 0;  // Flatbed.
  r.has_area = true;
  r.area_x0 = 0;
  r.area_y0 = 0;
  r.area_x1 = 2550;  // Letter width @300, 0-based (not a multiple of 16).
  r.area_y1 = 3300;
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.source, Source::kFlatbed);
  EXPECT_EQ(p.area.x0, 0);     // Corner-registered, unchanged.
  EXPECT_EQ(p.area.x1, 2544);  // 2550 aligned down to 2544 (the fringe fix).
  EXPECT_EQ(p.area.y0, 0);
  EXPECT_EQ(p.area.y1, 3300);
  EXPECT_EQ((p.area.x1 - p.area.x0) % 16, 0);
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

// Defensive: the scan dpi is clamped to the offered maximum, but the requested
// area (req.area_*) is in the REQUEST's dpi pixels. The ADF centering must derive
// the sensor width at that same request dpi so the requested width and the sensor
// width stay on one scale; using the clamped dpi would put them on two scales and
// drop the window to corner-register. Host requests never exceed the max today,
// so this only locks the invariant.
TEST(TranslateScanParamsTest, AdfCenteringUsesRequestDpiNotClampedDpi) {
  ScanRequest r;
  r.has_functional_unit = true;
  r.functional_unit = 3;  // Document feeder.
  r.has_resolution = true;
  r.resolution = 600;  // Above the max supplied below.
  r.has_area = true;
  r.area_x0 = 0;
  r.area_y0 = 0;
  r.area_x1 = 4000;  // Requested width, in 600-dpi pixels.
  r.area_y1 = 5000;
  // A low feeder cap forces the clamp; the request is a feeder scan, so the
  // feeder cap (300) is what applies here.
  const Params p =
      TranslateScanParams(r, ScanLimits{/*flatbed=*/2400, /*feeder=*/300});
  EXPECT_EQ(p.x_dpi, 300);  // Scan dpi is clamped to the max.
  // Sensor @600 = 6944, so centered x0 = (6944 - 4000) / 2 = 1472. Deriving the
  // sensor at the clamped 300 dpi (3472) would corner-register to 0 -- the bug.
  EXPECT_EQ(p.area.x0, 1472);
  EXPECT_EQ(p.area.x1, 1472 + 4000);  // Width preserved.
  EXPECT_EQ(p.area.y0, 0);            // Vertical bounds untouched.
  EXPECT_EQ(p.area.y1, 5000);
}

// ---------------------------------------------------------------------------
// Scan-width 16-px alignment (the JPEG-MCU right-edge fringe fix). The device
// encodes color as JPEG 4:2:0, whose chroma is sampled in 16-px MCUs, and
// Brother's native driver only ever requests widths that are exact multiples of
// 16. A width with a partial final MCU returns garbage chroma in the last
// columns (a magenta/rainbow fringe on the page's right edge), so
// TranslateScanParams rounds the requested width DOWN to the nearest multiple of
// 16 -- down, not up, so the window stays inside the paper.

// The resulting flatbed area width for a raw pixel width. x0 = 0, so the flatbed
// corner-registers and the returned width is directly comparable to the raw one.
int FlatbedAreaWidthFor(int raw_width) {
  ScanRequest r;
  r.has_functional_unit = true;
  r.functional_unit = 0;  // Flatbed: corner-registered, x0 stays 0.
  r.has_area = true;
  r.area_x0 = 0;
  r.area_y0 = 0;
  r.area_x1 = raw_width;
  r.area_y1 = 3300;
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.area.x0, 0);
  return p.area.x1 - p.area.x0;
}

// Representative paper widths (US-Letter and A4 at 100/300/600 dpi, in pixels)
// align DOWN to a multiple of 16. The Letter @300 case (2550 -> 2544) is the
// measured fringe case from the bug report.
TEST(ScanWidthAlignTest, AlignsRepresentativeWidthsDownTo16) {
  struct Case {
    int raw;
    int aligned;
    const char* what;
  };
  const Case cases[] = {
      {850, 848, "Letter 8.5in @100"},
      {2550, 2544, "Letter @300 (the measured fringe case)"},
      {5100, 5088, "Letter @600"},
      {827, 816, "A4 210mm @100"},
      {2480, 2480, "A4 @300 (already a multiple of 16 -> unchanged)"},
      {4961, 4960, "A4 @600"},
  };
  for (const Case& c : cases) {
    const int w = FlatbedAreaWidthFor(c.raw);
    EXPECT_EQ(w, c.aligned) << c.what;
    EXPECT_EQ(w % 16, 0) << c.what;      // Result is a whole number of MCUs.
    EXPECT_LE(w, c.raw) << c.what;       // Aligned DOWN, never up.
    EXPECT_LT(c.raw - w, 16) << c.what;  // Within 15 px of the request.
  }
}

// A raw width that is already a multiple of 16 is passed through unchanged.
TEST(ScanWidthAlignTest, AlreadyAlignedWidthUnchanged) {
  EXPECT_EQ(FlatbedAreaWidthFor(2544), 2544);  // 159*16.
  EXPECT_EQ(FlatbedAreaWidthFor(2512), 2512);  // 157*16 (Brother's Letter ADF).
  EXPECT_EQ(FlatbedAreaWidthFor(16), 16);      // Exactly one MCU.
}

// A raw width below 16 cannot be aligned down without underflowing, so it is
// left exactly as requested (degenerate, but never zeroed or made negative).
TEST(ScanWidthAlignTest, WidthBelow16LeftAsIs) {
  EXPECT_EQ(FlatbedAreaWidthFor(15), 15);
  EXPECT_EQ(FlatbedAreaWidthFor(8), 8);
  EXPECT_EQ(FlatbedAreaWidthFor(1), 1);
}

// Only the width is aligned; a non-16-aligned flatbed offset (x0) is untouched.
TEST(ScanWidthAlignTest, FlatbedAlignsWidthNotOffset) {
  ScanRequest r;  // No functional unit -> flatbed, corner-registered.
  r.has_area = true;
  r.area_x0 = 7;         // A non-16-aligned offset must survive untouched.
  r.area_y0 = 0;
  r.area_x1 = 7 + 2550;  // Raw width 2550.
  r.area_y1 = 3300;
  const Params p = TranslateScanParams(r, ScanLimits{});
  EXPECT_EQ(p.area.x0, 7);                  // Offset unchanged.
  EXPECT_EQ(p.area.x1 - p.area.x0, 2544);   // Width aligned down 2550 -> 2544.
  EXPECT_EQ((p.area.x1 - p.area.x0) % 16, 0);
}

// ADF/feeder path: the width is aligned first, THEN the centered x0 is computed
// from the ALIGNED width, so both the width and x1 - x0 are multiples of 16.
TEST(ScanWidthAlignTest, AdfCentersUsingAlignedWidth) {
  ScanRequest r;
  r.has_functional_unit = true;
  r.functional_unit = 3;  // Document feeder.
  r.has_resolution = true;
  r.resolution = 300;
  r.has_area = true;
  r.area_x0 = 0;
  r.area_y0 = 0;
  r.area_x1 = 2550;  // Letter raw width @300 -> aligns to 2544.
  r.area_y1 = 3300;
  const Params p = TranslateScanParams(r, ScanLimits{});
  const int width = p.area.x1 - p.area.x0;
  EXPECT_EQ(width, 2544);  // Aligned down from 2550.
  EXPECT_EQ(width % 16, 0);
  // x0 centers the ALIGNED width in the 3472-px sensor: (3472 - 2544)/2 = 464.
  // Centering the raw 2550 would give (3472 - 2550)/2 = 461, so x0 == 464 proves
  // the aligned width (not the raw one) drove the centering.
  EXPECT_EQ(p.area.x0, 464);
  EXPECT_EQ(p.area.x0, CenteredAdfX0(AdfSensorWidthAtDpi(300), 2544));
  EXPECT_EQ(p.area.y0, 0);  // Vertical bounds untouched.
  EXPECT_EQ(p.area.y1, 3300);
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

// A6 and the small photo sizes 3R (3.5x5) and 5R (5x7) match the non-contiguous
// SDK enum (ICScannerFunctionalUnits.h: ICScannerDocumentTypeA6 = 13; the photo
// region E=60, 3R=61, 4R=62, 5R=63). These are flatbed-only additions carrying
// no daemon/paper_size.cpp geometry (the host supplies the scan rectangle per
// document type).
TEST(DocumentTypeSizeConstantsTest, A6AndSmallPhotoValuesMatchSdkEnum) {
  EXPECT_EQ(kDocumentTypeA6, 13);
  EXPECT_EQ(kDocumentType3R, 61);
  EXPECT_EQ(kDocumentType5R, 63);
}

// The flatbed-only additions must not collide with the standard/JIS sizes or the
// platten default (a duplicate would drop a size from the dropdown).
TEST(DocumentTypeSizeConstantsTest, PhotoAndCardValuesDistinctFromExistingSizes) {
  const int values[] = {
      kDocumentTypeDefault,     kDocumentTypeA4,     kDocumentTypeUSLetter,
      kDocumentTypeUSLegal,     kDocumentTypeA5,     kDocumentTypeUSLedger,
      kDocumentTypeUSExecutive, kDocumentTypeA3,     kDocumentTypeJISB5,
      kDocumentTypeJISB4,       kDocumentTypeA6,     kDocumentType3R,
      kDocumentType4R,          kDocumentType5R,     kDocumentTypeBusinessCard};
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

// Every document type the feeder advertises fits the ADF envelope. Default (0)
// heads the list as the Auto / mixed-size choice; the ADF is plain-paper only,
// so the sub-148 mm sizes (A6, 3R, 5R, 4R, Business Card) stay off it.
TEST(SupportedSizesPerUnitTest, FeederSet) {
  const int feeder[] = {
      kDocumentTypeDefault,     kDocumentTypeUSLetter, kDocumentTypeUSLegal,
      kDocumentTypeA4,          kDocumentTypeUSLedger, kDocumentTypeA3,
      kDocumentTypeA5,          kDocumentTypeUSExecutive, kDocumentTypeJISB5,
      kDocumentTypeJISB4};
  auto contains = [&](int v) {
    for (int x : feeder)
      if (x == v) return true;
    return false;
  };
  // Default (Auto), A5, Executive, JIS B5, JIS B4 are present.
  EXPECT_TRUE(contains(kDocumentTypeDefault));
  EXPECT_TRUE(contains(kDocumentTypeA5));
  EXPECT_TRUE(contains(kDocumentTypeUSExecutive));
  EXPECT_TRUE(contains(kDocumentTypeJISB5));
  EXPECT_TRUE(contains(kDocumentTypeJISB4));
  // The sub-148 mm sizes are NOT on the feeder (plain-paper ADF only).
  EXPECT_FALSE(contains(kDocumentTypeA6));
  EXPECT_FALSE(contains(kDocumentType3R));
  EXPECT_FALSE(contains(kDocumentType5R));
  EXPECT_FALSE(contains(kDocumentType4R));
  EXPECT_FALSE(contains(kDocumentTypeBusinessCard));
}

// The flatbed set is the feeder set plus the flatbed-only sizes: A6, the photo
// sizes 3R/4R/5R, and Business Card. (Default heads both units.)
TEST(SupportedSizesPerUnitTest, FlatbedSet) {
  const int flatbed[] = {
      kDocumentTypeDefault,      kDocumentTypeUSLetter,    kDocumentTypeUSLegal,
      kDocumentTypeA4,           kDocumentTypeUSLedger,    kDocumentTypeA3,
      kDocumentTypeA5,           kDocumentTypeUSExecutive, kDocumentTypeJISB5,
      kDocumentTypeJISB4,        kDocumentTypeA6,          kDocumentType3R,
      kDocumentType4R,           kDocumentType5R,          kDocumentTypeBusinessCard};
  auto contains = [&](int v) {
    for (int x : flatbed)
      if (x == v) return true;
    return false;
  };
  EXPECT_TRUE(contains(kDocumentTypeDefault));
  // The flatbed-only additions.
  EXPECT_TRUE(contains(kDocumentTypeA6));
  EXPECT_TRUE(contains(kDocumentType3R));
  EXPECT_TRUE(contains(kDocumentType4R));
  EXPECT_TRUE(contains(kDocumentType5R));
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
// Per-unit ICAP_XRESOLUTION / ICAP_YRESOLUTION membership. The advertised list
// lives in scan_parameters.mm (BuildUnit -> ResolutionArray), which needs
// Foundation; these pure tests pin the INTENDED per-unit maxima so a regression
// there is caught without a device. Both units share the base set
// {100,150,200,300,400,600}; the high resolutions differ because the two sensors
// differ (Brother spec): the flatbed's optical maximum is 2400 dpi, the ADF's is
// 1200 dpi. So the flatbed advertises up to 2400 and the ADF up to 1200 (no
// 2400). The maxima are the same source caps TranslateScanParams clamps to
// (kMaxFlatbedDpi / kMaxFeederDpi), so the advertised list and the runtime clamp
// can never diverge.
TEST(ResolutionsPerUnitTest, FlatbedIncludes2400) {
  const int flatbed[] = {100, 150, 200, 300, 400, 600, 1200, 2400};
  auto contains = [&](int v) {
    for (int x : flatbed)
      if (x == v) return true;
    return false;
  };
  EXPECT_TRUE(contains(1200));
  EXPECT_TRUE(contains(2400));
  EXPECT_EQ(flatbed[std::size(flatbed) - 1], kMaxFlatbedDpi);  // 2400.
}

TEST(ResolutionsPerUnitTest, FeederStopsAt1200) {
  const int feeder[] = {100, 150, 200, 300, 400, 600, 1200};
  auto contains = [&](int v) {
    for (int x : feeder)
      if (x == v) return true;
    return false;
  };
  EXPECT_TRUE(contains(1200));
  EXPECT_FALSE(contains(2400));  // The ADF sensor tops out at 1200 dpi.
  EXPECT_EQ(feeder[std::size(feeder) - 1], kMaxFeederDpi);  // 1200.
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

// The corner output feeds TranslateScanParams: a converted positive rect flows
// through as the scan area, with the width aligned DOWN to a multiple of 16
// (200 -> 192, so x1 = 5 + 192 = 197). The offset and vertical bounds pass
// through untouched.
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
  EXPECT_EQ(p.area.x1, 197);  // 5 + (200 aligned down to 192).
  EXPECT_EQ(p.area.y1, 306);
  EXPECT_EQ((p.area.x1 - p.area.x0) % 16, 0);
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
  EXPECT_EQ(mode_for(1), ScanMode::kTrueGray);  // 8-bit gray -> GRAY256/RLENGTH.
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
