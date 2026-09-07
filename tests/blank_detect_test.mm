// Tests for daemon/blank_detect.h/.mm: host-side blank-page detection.
//
// IsBlankPage decodes a ScanResult, downscales it into a small gray bitmap,
// and judges the page blank when its ink coverage (fraction of pixels darker
// than the luminance threshold) falls below the blank epsilon. These tests
// build small synthetic pages -- all-white, a small dark block, and cases
// tuned right around each of the two tunable constants -- and assert the
// blank/non-blank verdict. kGray is covered directly; a kRgb pair (round-
// tripped through JPEG) confirms the decode path generalizes, and a couple of
// decode-failure cases confirm an un-inspectable page is never dropped. No
// scanned content is ever committed: every fixture is generated in-process.

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "blank_detect.h"
#include "brscan/scanner.h"
#include "brscan/types.h"

namespace brscan::scand {
namespace {

// Builds a kGray page (width x height, row-major, one byte/pixel) filled
// uniformly with `fill`.
brscan::ScanResult MakeGrayPage(int width, int height, uint8_t fill) {
  brscan::ScanResult page;
  page.format = brscan::PixelFormat::kGray;
  page.width = width;
  page.height = height;
  page.data.assign(static_cast<size_t>(width) * static_cast<size_t>(height),
                   fill);
  return page;
}

// A white kGray page with its first `dark_pixels` pixels (row-major) set to
// black -- a precise way to dial in an exact ink-coverage fraction.
brscan::ScanResult MakeGrayPageWithDarkPixels(int width, int height,
                                              int dark_pixels) {
  brscan::ScanResult page = MakeGrayPage(width, height, /*fill=*/255);
  for (int i = 0; i < dark_pixels; ++i) page.data[static_cast<size_t>(i)] = 0;
  return page;
}

// Encodes a solid-color kRgb page as a baseline JPEG (the on-the-wire form of
// a kRgb ScanResult -- CreateCGImageFromScanResult decodes it via ImageIO).
// Mirrors tests/image_transform_test.mm's EncodeRgbAsJpeg.
brscan::ScanResult MakeSolidRgbPage(int width, int height, uint8_t gray) {
  std::vector<uint8_t> rgba(static_cast<size_t>(width) *
                            static_cast<size_t>(height) * 4);
  for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
    rgba[i * 4 + 0] = gray;
    rgba[i * 4 + 1] = gray;
    rgba[i * 4 + 2] = gray;
    rgba[i * 4 + 3] = 255;
  }
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef ctx = CGBitmapContextCreate(
      rgba.data(), static_cast<size_t>(width), static_cast<size_t>(height), 8,
      static_cast<size_t>(width) * 4, cs,
      kCGBitmapByteOrderDefault | kCGImageAlphaNoneSkipLast);
  CGImageRef image = CGBitmapContextCreateImage(ctx);
  CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
  CGImageDestinationRef dest =
      CGImageDestinationCreateWithData(data, CFSTR("public.jpeg"), 1, nullptr);
  CGImageDestinationAddImage(dest, image, nullptr);
  CGImageDestinationFinalize(dest);
  const UInt8* bytes = CFDataGetBytePtr(data);
  std::vector<uint8_t> jpeg(bytes, bytes + CFDataGetLength(data));
  CFRelease(dest);
  CFRelease(data);
  CGImageRelease(image);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);

  brscan::ScanResult page;
  page.format = brscan::PixelFormat::kRgb;
  page.width = width;
  page.height = height;
  page.data = std::move(jpeg);
  return page;
}

// ---------------------------------------------------------------------
// kGray: the core blank / not-blank verdicts.
// ---------------------------------------------------------------------

TEST(IsBlankPageTest, AllWhiteGrayPageIsBlank) {
  EXPECT_TRUE(IsBlankPage(MakeGrayPage(100, 100, /*fill=*/255)));
}

TEST(IsBlankPageTest, AllBlackGrayPageIsNotBlank) {
  EXPECT_FALSE(IsBlankPage(MakeGrayPage(100, 100, /*fill=*/0)));
}

TEST(IsBlankPageTest, SmallDarkBlockIsNotBlank) {
  // A 20x20 black block on a 100x100 white page: 400/10000 = 4% coverage,
  // far above the 0.5% blank epsilon.
  brscan::ScanResult page = MakeGrayPage(100, 100, /*fill=*/255);
  for (int y = 0; y < 20; ++y) {
    for (int x = 0; x < 20; ++x) {
      page.data[static_cast<size_t>(y) * 100 + static_cast<size_t>(x)] = 0;
    }
  }
  EXPECT_FALSE(IsBlankPage(page));
}

// ---------------------------------------------------------------------
// Pin kInkLuminanceThreshold (~0.75*255 = 191): a uniform page just lighter
// than the threshold is all "paper" (blank); just darker is all "ink" (not
// blank). This would flip if the threshold moved across 191.
// ---------------------------------------------------------------------

TEST(IsBlankPageTest, UniformLightGrayAboveInkThresholdIsBlank) {
  // 210 > 191: no pixel counts as ink, so coverage is 0 -> blank.
  EXPECT_TRUE(IsBlankPage(MakeGrayPage(100, 100, /*fill=*/210)));
}

TEST(IsBlankPageTest, UniformMidGrayBelowInkThresholdIsNotBlank) {
  // 170 < 191: every pixel counts as ink, so coverage is 100% -> not blank.
  EXPECT_FALSE(IsBlankPage(MakeGrayPage(100, 100, /*fill=*/170)));
}

// ---------------------------------------------------------------------
// Pin kBlankCoverageEpsilon (0.5%): on a 100x100 (10000-pixel) page, 50 dark
// pixels is exactly the boundary. 40 (0.4%) stays blank; 60 (0.6%) does not.
// This would flip if the epsilon moved across 0.5%.
// ---------------------------------------------------------------------

TEST(IsBlankPageTest, CoverageJustBelowEpsilonIsBlank) {
  EXPECT_TRUE(IsBlankPage(MakeGrayPageWithDarkPixels(100, 100,
                                                     /*dark_pixels=*/40)));
}

TEST(IsBlankPageTest, CoverageJustAboveEpsilonIsNotBlank) {
  EXPECT_FALSE(IsBlankPage(MakeGrayPageWithDarkPixels(100, 100,
                                                      /*dark_pixels=*/60)));
}

// ---------------------------------------------------------------------
// kRgb: the JPEG-decode path reaches the same verdicts (JPEG is lossy, so
// these use solid pages with wide margins on either side of the threshold).
// ---------------------------------------------------------------------

TEST(IsBlankPageTest, SolidWhiteRgbPageIsBlank) {
  EXPECT_TRUE(IsBlankPage(MakeSolidRgbPage(120, 90, /*gray=*/255)));
}

TEST(IsBlankPageTest, SolidBlackRgbPageIsNotBlank) {
  EXPECT_FALSE(IsBlankPage(MakeSolidRgbPage(120, 90, /*gray=*/0)));
}

// ---------------------------------------------------------------------
// Decode/geometry failures: a page that cannot be inspected is never dropped.
// ---------------------------------------------------------------------

TEST(IsBlankPageTest, ZeroDimensionPageIsTreatedAsNotBlank) {
  brscan::ScanResult page;
  page.format = brscan::PixelFormat::kGray;
  page.width = 0;
  page.height = 0;
  EXPECT_FALSE(IsBlankPage(page));
}

TEST(IsBlankPageTest, UndecodableRgbPageIsTreatedAsNotBlank) {
  // Non-JPEG bytes for a kRgb page: CreateCGImageFromScanResult fails to
  // decode, so IsBlankPage keeps the page (returns false) rather than
  // dropping something it could not measure.
  brscan::ScanResult page;
  page.format = brscan::PixelFormat::kRgb;
  page.width = 32;
  page.height = 32;
  page.data = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
  EXPECT_FALSE(IsBlankPage(page));
}

}  // namespace
}  // namespace brscan::scand
