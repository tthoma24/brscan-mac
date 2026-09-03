#include "response.h"

#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include "brscan/types.h"

namespace {

std::vector<uint8_t> ReadFixture(const std::string& name) {
  const std::string path =
      std::string(BRSCAN_FIXTURES_DIR) + "/responses/" + name;
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
}

}  // namespace

// --- ParseOffer -------------------------------------------------------

TEST(ParseOffer, PerScanGrant100Dpi) {
  const auto offer = brscan::ParseOffer("100,100,2,292,1153,427,1684,");
  ASSERT_TRUE(offer.has_value());
  EXPECT_EQ(offer->x_dpi, 100);
  EXPECT_EQ(offer->y_dpi, 100);
  EXPECT_EQ(offer->width_px, 1153);
  EXPECT_EQ(offer->height_px, 1684);
}

TEST(ParseOffer, PerScanGrant300Dpi) {
  const auto offer = brscan::ParseOffer("300,300,2,292,3460,427,5052,");
  ASSERT_TRUE(offer.has_value());
  EXPECT_EQ(offer->x_dpi, 300);
  EXPECT_EQ(offer->y_dpi, 300);
  EXPECT_EQ(offer->width_px, 3460);
  EXPECT_EQ(offer->height_px, 5052);
}

TEST(ParseOffer, CapabilityProbeReply) {
  // flag=1 rather than 2, and no per-scan ymax (0,0).
  const auto offer = brscan::ParseOffer("2400,1200,1,292,27685,0,0,");
  ASSERT_TRUE(offer.has_value());
  EXPECT_EQ(offer->x_dpi, 2400);
  EXPECT_EQ(offer->y_dpi, 1200);
  EXPECT_EQ(offer->width_px, 27685);
  EXPECT_EQ(offer->height_px, 0);
}

TEST(ParseOffer, MissingTrailingCommaIsMalformed) {
  EXPECT_FALSE(brscan::ParseOffer("100,100,2,292,1153,427,1684").has_value());
}

TEST(ParseOffer, TooFewFieldsIsMalformed) {
  EXPECT_FALSE(brscan::ParseOffer("100,100,2,").has_value());
}

TEST(ParseOffer, NonNumericFieldIsMalformed) {
  EXPECT_FALSE(
      brscan::ParseOffer("100,100,2,292,abc,427,1684,").has_value());
}

TEST(ParseOffer, EmptyStringIsMalformed) {
  EXPECT_FALSE(brscan::ParseOffer("").has_value());
}

// --- ParseBlockHeader ---------------------------------------------------

TEST(ParseBlockHeader, GrayHeaderWidth) {
  // 00 40 07 00 01 00 84 00 00 00 00 90 0d -- confirmed width = 3472.
  const std::vector<uint8_t> header = {0x00, 0x40, 0x07, 0x00, 0x01, 0x00,
                                        0x84, 0x00, 0x00, 0x00, 0x00, 0x90,
                                        0x0d};
  const auto parsed = brscan::ParseBlockHeader(header.data(), header.size());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->width, 3472);
  EXPECT_EQ(parsed->type, 0x40);
}

TEST(ParseBlockHeader, RlengthHeaderType) {
  // 00 42 07 00 01 00 84 00 00 00 00 08 00 -- a TEXT/ERRDIF/GRAY256 row
  // block: type 0x42 (RLENGTH-compressed), 8-byte compressed payload.
  // See reference/streams/modes_text_in.bin's first row (a blank/white
  // scanline) via reference/protocol-notes-modes.md.
  const std::vector<uint8_t> header = {0x00, 0x42, 0x07, 0x00, 0x01, 0x00,
                                        0x84, 0x00, 0x00, 0x00, 0x00, 0x08,
                                        0x00};
  const auto parsed = brscan::ParseBlockHeader(header.data(), header.size());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->width, 8);
  EXPECT_EQ(parsed->type, 0x42);
}

TEST(ParseBlockHeader, GrayHeaderFixtureFile) {
  const auto bytes = ReadFixture("gray-block-header.bin");
  ASSERT_GE(bytes.size(), 13u);
  const auto parsed = brscan::ParseBlockHeader(bytes.data(), 13);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->width, 3472);
}

TEST(ParseBlockHeader, ColorHeaderParses) {
  // 00 64 07 00 01 00 84 c0 01 00 00 f4 ff -- width field here is the
  // JPEG-length sentinel (0xfff4), not a pixel width; parsing still
  // succeeds because the anchor bytes (offset 2, offset 6) match.
  const std::vector<uint8_t> header = {0x00, 0x64, 0x07, 0x00, 0x01, 0x00,
                                        0x84, 0xc0, 0x01, 0x00, 0x00, 0xf4,
                                        0xff};
  const auto parsed = brscan::ParseBlockHeader(header.data(), header.size());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->width, 0xfff4);
  EXPECT_EQ(parsed->type, 0x64);
}

TEST(ParseBlockHeader, TooShortIsMalformed) {
  const std::vector<uint8_t> header = {0x00, 0x40, 0x07, 0x00, 0x01};
  EXPECT_FALSE(
      brscan::ParseBlockHeader(header.data(), header.size()).has_value());
}

TEST(ParseBlockHeader, BadAnchorIsMalformed) {
  // Same as the gray header but with offset 6 corrupted (0x84 -> 0x00).
  const std::vector<uint8_t> header = {0x00, 0x40, 0x07, 0x00, 0x01, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x90,
                                        0x0d};
  EXPECT_FALSE(
      brscan::ParseBlockHeader(header.data(), header.size()).has_value());
}

// --- DecodeGrayRaw -------------------------------------------------------

TEST(DecodeGrayRaw, WhiteBufferRoundTrips) {
  const int width = 4;
  const int height = 3;
  const std::vector<uint8_t> raw(width * height, 0xff);
  brscan::Image image;
  const auto status = brscan::DecodeGrayRaw(width, height, raw.data(),
                                             raw.size(), &image);
  ASSERT_EQ(status, brscan::Status::kOk);
  EXPECT_EQ(image.width, width);
  EXPECT_EQ(image.height, height);
  EXPECT_EQ(image.format, brscan::PixelFormat::kGray);
  ASSERT_EQ(image.pixels.size(), raw.size());
  for (uint8_t px : image.pixels) EXPECT_EQ(px, 0xff);
}

TEST(DecodeGrayRaw, ShortBufferIsIncomplete) {
  const int width = 4;
  const int height = 3;
  // One byte short of width * height: models a cancelled/stalled scan.
  const std::vector<uint8_t> raw(width * height - 1, 0xff);
  brscan::Image image;
  const auto status = brscan::DecodeGrayRaw(width, height, raw.data(),
                                             raw.size(), &image);
  EXPECT_EQ(status, brscan::Status::kProtocolError);
}
