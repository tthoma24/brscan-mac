// Tests for daemon/image_transform.h/.mm: the high-speed page rotation.
//
// RotatePortrait rotates a landscape-fed high-speed page 90 degrees back to
// portrait. These tests build small synthetic ScanResults with an
// asymmetric corner marker and assert -- with an exact per-pixel check for
// the raster formats (kGray, kBitonal) -- that the marker lands in the
// rotated-expected corner and that the dimensions swap. The kRgb case round-
// trips through JPEG (lossy), so it checks the marker's corner with a
// tolerance instead. No scanned content is ever committed: every fixture is
// generated in-process.

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "action_ocr.h"
#include "brscan/scanner.h"
#include "brscan/types.h"
#include "image_transform.h"

namespace brscan::scand {
namespace {

// A dark sample (marker) vs a light one (background), in 8-bit gray.
constexpr uint8_t kDark = 0;
constexpr uint8_t kLight = 255;

// Builds a kGray page (width x height, row-major, one byte/pixel), all light
// except a `block` x `block` dark square in the TOP-LEFT corner.
brscan::ScanResult MakeGrayPageWithTopLeftMarker(int width, int height,
                                                 int block) {
  brscan::ScanResult page;
  page.format = brscan::PixelFormat::kGray;
  page.width = width;
  page.height = height;
  page.data.assign(static_cast<size_t>(width) * static_cast<size_t>(height),
                   kLight);
  for (int y = 0; y < block; ++y) {
    for (int x = 0; x < block; ++x) {
      page.data[static_cast<size_t>(y) * static_cast<size_t>(width) +
                static_cast<size_t>(x)] = kDark;
    }
  }
  return page;
}

// True if the pixel at (row, col) of a `width`-wide, `height`-tall gray
// raster is expected dark for a TOP-RIGHT `block` x `block` marker -- where a
// 90-degree-clockwise rotation sends the input's top-left marker.
bool ExpectedDarkTopRight(int row, int col, int width, int height, int block) {
  (void)height;
  return row < block && col >= width - block;
}

// ---------------------------------------------------------------------
// kGray: exact per-pixel rotation.
// ---------------------------------------------------------------------

TEST(RotatePortraitTest, GraySwapsDimsAndMovesMarkerToTopRight) {
  constexpr int kW = 4, kH = 6, kBlock = 2;
  const brscan::ScanResult page = MakeGrayPageWithTopLeftMarker(kW, kH, kBlock);

  const std::optional<brscan::ScanResult> out = RotatePortrait(page);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->format, brscan::PixelFormat::kGray);
  EXPECT_EQ(out->width, kH);   // 90-degree rotation swaps the axes.
  EXPECT_EQ(out->height, kW);
  ASSERT_EQ(out->data.size(),
            static_cast<size_t>(out->width) * static_cast<size_t>(out->height));

  // Exact pixel check: the top-left marker must now be at the top-right,
  // every pixel dark-or-light exactly as a 90-degree CW rotation dictates.
  for (int row = 0; row < out->height; ++row) {
    for (int col = 0; col < out->width; ++col) {
      const uint8_t sample =
          out->data[static_cast<size_t>(row) * static_cast<size_t>(out->width) +
                    static_cast<size_t>(col)];
      if (ExpectedDarkTopRight(row, col, out->width, out->height, kBlock)) {
        EXPECT_EQ(sample, kDark) << "row=" << row << " col=" << col;
      } else {
        EXPECT_EQ(sample, kLight) << "row=" << row << " col=" << col;
      }
    }
  }
}

// ---------------------------------------------------------------------
// kBitonal: exact per-pixel rotation, exercising the 1-bpp re-pack path.
// ---------------------------------------------------------------------

TEST(RotatePortraitTest, BitonalSwapsDimsAndMovesMarkerToTopRight) {
  // 8x4 so the width is byte-aligned (row_bytes = 1); top-left 2x2 black
  // (bit 1 = black, MSB-first, per PixelFormat::kBitonal).
  constexpr int kW = 8, kH = 4, kBlock = 2;
  brscan::ScanResult page;
  page.format = brscan::PixelFormat::kBitonal;
  page.width = kW;
  page.height = kH;
  const size_t row_bytes = (static_cast<size_t>(kW) + 7) / 8;
  page.data.assign(row_bytes * static_cast<size_t>(kH), 0x00);
  // Rows 0,1, cols 0,1 black: bits 0x80>>0 | 0x80>>1 = 0xC0.
  page.data[0] = 0xC0;
  page.data[1] = 0xC0;

  const std::optional<brscan::ScanResult> out = RotatePortrait(page);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->format, brscan::PixelFormat::kBitonal);
  EXPECT_EQ(out->width, kH);   // 4
  EXPECT_EQ(out->height, kW);  // 8

  const size_t out_row_bytes = (static_cast<size_t>(out->width) + 7) / 8;
  ASSERT_EQ(out->data.size(), out_row_bytes * static_cast<size_t>(out->height));

  // Marker now at top-right: rows 0,1, cols 2,3 -> bits 0x80>>2 | 0x80>>3 =
  // 0x30. Every other row byte is 0x00.
  for (int row = 0; row < out->height; ++row) {
    const uint8_t byte = out->data[static_cast<size_t>(row) * out_row_bytes];
    if (row < kBlock) {
      EXPECT_EQ(byte, 0x30) << "row=" << row;
    } else {
      EXPECT_EQ(byte, 0x00) << "row=" << row;
    }
  }
}

// ---------------------------------------------------------------------
// kRgb: dims swap + marker corner (tolerant, JPEG is lossy).
// ---------------------------------------------------------------------

// Encodes a raw interleaved-RGB buffer (width x height, 3 bytes/pixel) as a
// baseline JPEG -- the on-the-wire form of a kRgb ScanResult.
std::vector<uint8_t> EncodeRgbAsJpeg(const std::vector<uint8_t>& rgb, int width,
                                     int height) {
  // Widen to RGBA (skip-alpha) for a supported CGBitmapContext layout.
  std::vector<uint8_t> rgba(static_cast<size_t>(width) *
                            static_cast<size_t>(height) * 4);
  for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
    rgba[i * 4 + 0] = rgb[i * 3 + 0];
    rgba[i * 4 + 1] = rgb[i * 3 + 1];
    rgba[i * 4 + 2] = rgb[i * 3 + 2];
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
  std::vector<uint8_t> out(bytes, bytes + CFDataGetLength(data));
  CFRelease(dest);
  CFRelease(data);
  CGImageRelease(image);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return out;
}

// Decodes a page to an 8-bit gray raster (width x height of the decoded
// image) via CreateCGImageFromScanResult, for corner sampling.
std::vector<uint8_t> DecodeToGray8(const brscan::ScanResult& page, int* width,
                                   int* height) {
  CGImageRef image = brscan::CreateCGImageFromScanResult(page);
  EXPECT_NE(image, nullptr);
  const int w = static_cast<int>(CGImageGetWidth(image));
  const int h = static_cast<int>(CGImageGetHeight(image));
  *width = w;
  *height = h;
  std::vector<uint8_t> gray(static_cast<size_t>(w) * static_cast<size_t>(h));
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
  CGContextRef ctx = CGBitmapContextCreate(
      gray.data(), static_cast<size_t>(w), static_cast<size_t>(h), 8,
      static_cast<size_t>(w), cs,
      kCGBitmapByteOrderDefault | kCGImageAlphaNone);
  CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
  CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  CGImageRelease(image);
  return gray;
}

TEST(RotatePortraitTest, RgbSwapsDimsAndMovesMarkerToTopRight) {
  constexpr int kW = 8, kH = 12;
  // Raw RGB: all white, with the top-left quadrant dark.
  std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3, 255);
  for (int y = 0; y < kH / 2; ++y) {
    for (int x = 0; x < kW / 2; ++x) {
      const size_t base = (static_cast<size_t>(y) * kW + x) * 3;
      rgb[base + 0] = 0;
      rgb[base + 1] = 0;
      rgb[base + 2] = 0;
    }
  }

  brscan::ScanResult page;
  page.format = brscan::PixelFormat::kRgb;
  page.width = kW;
  page.height = kH;
  page.data = EncodeRgbAsJpeg(rgb, kW, kH);

  const std::optional<brscan::ScanResult> out = RotatePortrait(page);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->format, brscan::PixelFormat::kRgb);
  EXPECT_EQ(out->width, kH);
  EXPECT_EQ(out->height, kW);
  ASSERT_FALSE(out->data.empty());

  int dw = 0, dh = 0;
  const std::vector<uint8_t> gray = DecodeToGray8(*out, &dw, &dh);
  ASSERT_EQ(dw, kH);
  ASSERT_EQ(dh, kW);

  const auto at = [&](int row, int col) {
    return gray[static_cast<size_t>(row) * static_cast<size_t>(dw) +
                static_cast<size_t>(col)];
  };
  // The dark top-left quadrant must now be the top-RIGHT quadrant. Sample a
  // pixel well inside each corner (2px in) to dodge JPEG edge ringing.
  EXPECT_LT(at(2, dw - 3), 100);  // top-right: dark.
  EXPECT_GT(at(2, 2), 155);        // top-left: light.
  EXPECT_GT(at(dh - 3, dw - 3), 155);  // bottom-right: light.
  EXPECT_GT(at(dh - 3, 2), 155);       // bottom-left: light.
}

// ---------------------------------------------------------------------
// Failure handling.
// ---------------------------------------------------------------------

TEST(RotatePortraitTest, ZeroDimensionPageReturnsNullopt) {
  brscan::ScanResult page;
  page.format = brscan::PixelFormat::kGray;
  page.width = 0;
  page.height = 0;
  EXPECT_FALSE(RotatePortrait(page).has_value());
}

TEST(RotatePortraitTest, TruncatedGrayPageReturnsNullopt) {
  // Claims 4x6 but supplies too few bytes: CreateCGImageFromScanResult
  // rejects it, so RotatePortrait returns nullopt rather than reading OOB.
  brscan::ScanResult page;
  page.format = brscan::PixelFormat::kGray;
  page.width = 4;
  page.height = 6;
  page.data.assign(3, kLight);
  EXPECT_FALSE(RotatePortrait(page).has_value());
}

}  // namespace
}  // namespace brscan::scand
