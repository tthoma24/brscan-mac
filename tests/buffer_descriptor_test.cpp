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

// ---------------------------------------------------------------------
// DescribeBand: per-band image-info args + the stride guard (Task 18b).
// ---------------------------------------------------------------------

TEST(DescribeBandTest, RgbBandCarriesFullPageGeometryAndBandRows) {
  // A 16-row band starting at row 32 of a 100x50 RGB page. Stride = 100*3.
  const int64_t stride = static_cast<int64_t>(100) * 3;
  const auto info = DescribeBand(PixelFormat::kRgb, /*full_width=*/100,
                                 /*full_height=*/50, /*start_row=*/32,
                                 /*num_rows=*/16, /*band_size=*/stride * 16);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->width, 100);           // FULL page width.
  EXPECT_EQ(info->height, 50);           // FULL page height.
  EXPECT_EQ(info->bytes_per_row, stride);
  EXPECT_EQ(info->data_start_row, 32);
  EXPECT_EQ(info->data_number_of_rows, 16);
  EXPECT_EQ(info->data_size, stride * 16);
}

TEST(DescribeBandTest, GrayAndBitonalStridesMatchDescribeBuffer) {
  const auto gray = DescribeBand(PixelFormat::kGray, 200, 300, 0, 16,
                                 static_cast<size_t>(200) * 16);
  ASSERT_TRUE(gray.has_value());
  EXPECT_EQ(gray->bytes_per_row, 200);
  EXPECT_EQ(gray->data_size, static_cast<int64_t>(200) * 16);

  // Bitonal stride rounds up to whole bytes: (201 + 7) / 8 = 26.
  const int64_t bstride = (201 + 7) / 8;
  const auto bit = DescribeBand(PixelFormat::kBitonal, 201, 300, 16, 16,
                                static_cast<size_t>(bstride) * 16);
  ASSERT_TRUE(bit.has_value());
  EXPECT_EQ(bit->bytes_per_row, bstride);
  EXPECT_EQ(bit->data_size, bstride * 16);
}

TEST(DescribeBandTest, LastBandMayBeShorterThanTheOthers) {
  // 50-row page in 16-row bands: the final band covers rows 48..49 (2 rows).
  const int64_t stride = static_cast<int64_t>(100) * 3;
  const auto info = DescribeBand(PixelFormat::kRgb, 100, 50, 48, 2,
                                 static_cast<size_t>(stride) * 2);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->data_start_row, 48);
  EXPECT_EQ(info->data_number_of_rows, 2);
  EXPECT_EQ(info->data_size, stride * 2);
}

TEST(DescribeBandTest, StrideMismatchIsRejected) {
  // band_size that is not stride * num_rows renders nothing on the host, so the
  // guard must reject it (the module drops such a band).
  const int64_t stride = static_cast<int64_t>(100) * 3;
  EXPECT_EQ(DescribeBand(PixelFormat::kRgb, 100, 50, 0, 16, stride * 16 - 1),
            std::nullopt);
  EXPECT_EQ(DescribeBand(PixelFormat::kRgb, 100, 50, 0, 16, stride * 16 + 1),
            std::nullopt);
  EXPECT_EQ(DescribeBand(PixelFormat::kRgb, 100, 50, 0, 16, 0), std::nullopt);
}

TEST(DescribeBandTest, BandOutsidePageIsRejected) {
  const int64_t stride = static_cast<int64_t>(100) * 3;
  // start_row + num_rows runs past full_height (48 + 16 > 50).
  EXPECT_EQ(DescribeBand(PixelFormat::kRgb, 100, 50, 48, 16,
                         static_cast<size_t>(stride) * 16),
            std::nullopt);
  // Negative start row.
  EXPECT_EQ(DescribeBand(PixelFormat::kRgb, 100, 50, -1, 16,
                         static_cast<size_t>(stride) * 16),
            std::nullopt);
  // Zero / negative row count.
  EXPECT_EQ(DescribeBand(PixelFormat::kRgb, 100, 50, 0, 0, 0), std::nullopt);
}

TEST(DescribeBandTest, InvalidPageGeometryIsRejected) {
  EXPECT_EQ(DescribeBand(PixelFormat::kRgb, 0, 50, 0, 16, 0), std::nullopt);
  EXPECT_EQ(DescribeBand(PixelFormat::kGray, 100, 0, 0, 16, 0), std::nullopt);
}

}  // namespace
}  // namespace brscan::ica
