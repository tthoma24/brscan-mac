// Tests for the per-page host buffer descriptor (ica-module/buffer_descriptor.h).
//
// A pure, hermetic unit: no ICADevices, no framework, no device. It maps one
// scanned page's shape (PixelFormat + width/height) to the host buffer
// parameters (bpp/spp/stride/size/color-space) recommended by
// PLAN-2-DESIGN.md section G.

#include "buffer_descriptor.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "brscan/scanner.h"
#include "brscan/types.h"

namespace brscan::ica {
namespace {

// ---------------------------------------------------------------------
// Per-format bpp / spp / stride / color-space, at a small representative size.
// ---------------------------------------------------------------------

TEST(DescribeBufferTest, RgbFormat) {
  const auto d = DescribeBuffer(PixelFormat::kRgb, 100, 50);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->bits_per_pixel, 24);
  EXPECT_EQ(d->samples_per_pixel, 3);
  EXPECT_EQ(d->bytes_per_row, 100 * 3);
  EXPECT_EQ(d->expected_byte_count, static_cast<int64_t>(100) * 3 * 50);
  EXPECT_EQ(d->color_space, ColorSpace::kDeviceRGB);
}

TEST(DescribeBufferTest, GrayFormat) {
  const auto d = DescribeBuffer(PixelFormat::kGray, 100, 50);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->bits_per_pixel, 8);
  EXPECT_EQ(d->samples_per_pixel, 1);
  EXPECT_EQ(d->bytes_per_row, 100);
  EXPECT_EQ(d->expected_byte_count, static_cast<int64_t>(100) * 50);
  EXPECT_EQ(d->color_space, ColorSpace::kDeviceGray);
}

TEST(DescribeBufferTest, BitonalFormat) {
  const auto d = DescribeBuffer(PixelFormat::kBitonal, 100, 50);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->bits_per_pixel, 1);
  EXPECT_EQ(d->samples_per_pixel, 1);
  EXPECT_EQ(d->bytes_per_row, (100 + 7) / 8);  // 13 bytes for 100 px.
  EXPECT_EQ(d->expected_byte_count, static_cast<int64_t>((100 + 7) / 8) * 50);
  EXPECT_EQ(d->color_space, ColorSpace::kBlackWhite1Bit);
}

// ---------------------------------------------------------------------
// Bitonal stride rounding: (width + 7) / 8, MSB-first byte padding.
// ---------------------------------------------------------------------

TEST(DescribeBufferTest, BitonalStrideRoundsUpToWholeBytes) {
  // width -> expected stride bytes.
  struct Case {
    int width;
    int64_t stride;
  };
  const Case cases[] = {
      {1, 1}, {7, 1}, {8, 1}, {9, 2}, {15, 2}, {16, 2}, {17, 3}, {100, 13},
  };
  for (const Case& c : cases) {
    const auto d = DescribeBuffer(PixelFormat::kBitonal, c.width, 1);
    ASSERT_TRUE(d.has_value()) << "width=" << c.width;
    EXPECT_EQ(d->bytes_per_row, c.stride) << "width=" << c.width;
    EXPECT_EQ(d->expected_byte_count, c.stride) << "width=" << c.width;
  }
}

// ---------------------------------------------------------------------
// byte-count == stride * height for representative real page sizes.
// ---------------------------------------------------------------------

TEST(DescribeBufferTest, LetterAt300Rgb) {
  // US Letter @ 300 dpi = 2550 x 3300 px.
  const auto d = DescribeBuffer(PixelFormat::kRgb, 2550, 3300);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->bytes_per_row, static_cast<int64_t>(2550) * 3);
  EXPECT_EQ(d->expected_byte_count, static_cast<int64_t>(2550) * 3 * 3300);
}

TEST(DescribeBufferTest, LegalAt300Gray) {
  // US Legal @ 300 dpi = 2550 x 4200 px.
  const auto d = DescribeBuffer(PixelFormat::kGray, 2550, 4200);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->bytes_per_row, 2550);
  EXPECT_EQ(d->expected_byte_count, static_cast<int64_t>(2550) * 4200);
}

TEST(DescribeBufferTest, LedgerAt600RgbUses64BitAccumulator) {
  // 11x17 ledger @ 600 dpi = 6600 x 10200 px. Decoded RGB byte count is
  // 6600 * 3 * 10200 = 201,960,000 -- past nothing here, but the row stride
  // (19,800) times height must accumulate in 64-bit to be exact.
  const int width = 6600;
  const int height = 10200;
  const auto d = DescribeBuffer(PixelFormat::kRgb, width, height);
  ASSERT_TRUE(d.has_value());
  const int64_t stride = static_cast<int64_t>(width) * 3;
  EXPECT_EQ(d->bytes_per_row, stride);
  EXPECT_EQ(d->expected_byte_count, stride * height);
  EXPECT_EQ(d->expected_byte_count, static_cast<int64_t>(201960000));
}

// ---------------------------------------------------------------------
// Invalid geometry is rejected cleanly.
// ---------------------------------------------------------------------

TEST(DescribeBufferTest, NonPositiveGeometryReturnsNullopt) {
  EXPECT_EQ(DescribeBuffer(PixelFormat::kRgb, 0, 100), std::nullopt);
  EXPECT_EQ(DescribeBuffer(PixelFormat::kRgb, 100, 0), std::nullopt);
  EXPECT_EQ(DescribeBuffer(PixelFormat::kRgb, -1, 100), std::nullopt);
  EXPECT_EQ(DescribeBuffer(PixelFormat::kGray, 100, -5), std::nullopt);
  EXPECT_EQ(DescribeBuffer(PixelFormat::kBitonal, 0, 0), std::nullopt);
}

// ---------------------------------------------------------------------
// kRgb expected size is the DECODED RGB size, independent of any JPEG length.
// ---------------------------------------------------------------------

TEST(DescribeBufferTest, RgbSizeIsDecodedRgbNotJpegPayloadLength) {
  // A ScanResult whose data is a tiny "JPEG" of an arbitrary, unrelated
  // length. The descriptor's expected_byte_count must be the decoded RGB
  // size from geometry (width*3*height), NOT data.size().
  ScanResult result;
  result.format = PixelFormat::kRgb;
  result.width = 640;
  result.height = 480;
  result.data = std::vector<uint8_t>(37, 0xAB);  // Nonsense compressed length.

  const auto d = DescribeBuffer(result);
  ASSERT_TRUE(d.has_value());
  const int64_t decoded = static_cast<int64_t>(640) * 3 * 480;  // 921,600.
  EXPECT_EQ(d->expected_byte_count, decoded);
  EXPECT_NE(d->expected_byte_count, static_cast<int64_t>(result.data.size()));
}

// ---------------------------------------------------------------------
// The ScanResult overload agrees with the primitive one and ignores data.
// ---------------------------------------------------------------------

TEST(DescribeBufferTest, ScanResultOverloadMatchesPrimitiveOverload) {
  ScanResult result;
  result.format = PixelFormat::kBitonal;
  result.width = 2550;
  result.height = 3300;
  result.data.clear();  // Empty data must not affect the descriptor.

  const auto from_result = DescribeBuffer(result);
  const auto from_prims = DescribeBuffer(PixelFormat::kBitonal, 2550, 3300);
  ASSERT_TRUE(from_result.has_value());
  ASSERT_TRUE(from_prims.has_value());
  EXPECT_EQ(from_result->bits_per_pixel, from_prims->bits_per_pixel);
  EXPECT_EQ(from_result->samples_per_pixel, from_prims->samples_per_pixel);
  EXPECT_EQ(from_result->bytes_per_row, from_prims->bytes_per_row);
  EXPECT_EQ(from_result->expected_byte_count, from_prims->expected_byte_count);
  EXPECT_EQ(from_result->color_space, from_prims->color_space);
}

}  // namespace
}  // namespace brscan::ica
