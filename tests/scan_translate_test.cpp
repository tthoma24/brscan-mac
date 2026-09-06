// Tests for the host scan-request -> brscan::Params translation
// (ica-module/scan_translate.h).
//
// A pure, hermetic unit: no ICADevices, no Foundation, no device. It maps the
// host's selection (resolution, pixel type, functional unit, duplex,
// brightness, contrast, area) onto a brscan::Params per PLAN-2-DESIGN.md's
// "Scan-parameter mapping" table, including resolution clamping, the
// full-area default, and the button_flow == false invariant.

#include "scan_translate.h"

#include <gtest/gtest.h>

#include "brscan/types.h"

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

}  // namespace
}  // namespace brscan::ica
