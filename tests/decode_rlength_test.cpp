#include "decode_rlength.h"

#include <vector>

#include <gtest/gtest.h>

#include "brscan/types.h"

// Test vectors for the literal/repeat/no-op control-byte cases (WhiteLine,
// MixedRunThenLiteral, LargeRunOfMaxLength) are standard/textbook PackBits
// cases (hand-built to exercise each control-byte range), not transcribed
// from any captured or third-party file. The BlankTextRow case below is
// derived from this project's own capture but is a blank/white scanline,
// not decodable scanned content; see docs/PROTOCOL.md and PROVENANCE.md.

TEST(DecodeRlengthRow, WhiteLine) {
  // "0x81 0xFF -> repeat 0xFF 128 times" x3, then "0xF1 0xFF -> repeat
  // 0xFF 16 times": 3*128 + 16 = 400 bytes, all 0xFF.
  const std::vector<uint8_t> in = {0x81, 0xFF, 0x81, 0xFF,
                                    0x81, 0xFF, 0xF1, 0xFF};
  std::vector<uint8_t> out(400);
  const auto status =
      brscan::DecodeRlengthRow(in.data(), in.size(), out.data(), out.size());
  ASSERT_EQ(status, brscan::Status::kOk);
  for (uint8_t b : out) EXPECT_EQ(b, 0xFF);
}

TEST(DecodeRlengthRow, MixedRunThenLiteral) {
  // 0xFE 0xAA -> repeat 0xAA 3 times; 0x01 0x55 0x66 -> literal 2 bytes.
  const std::vector<uint8_t> in = {0xFE, 0xAA, 0x01, 0x55, 0x66};
  std::vector<uint8_t> out(5);
  const auto status =
      brscan::DecodeRlengthRow(in.data(), in.size(), out.data(), out.size());
  ASSERT_EQ(status, brscan::Status::kOk);
  const std::vector<uint8_t> want = {0xAA, 0xAA, 0xAA, 0x55, 0x66};
  EXPECT_EQ(out, want);
}

TEST(DecodeRlengthRow, LargeRunOfMaxLength) {
  // Control byte 0x81 is the largest repeat count PackBits can encode in
  // one run: 257 - 0x81 = 128.
  const std::vector<uint8_t> in = {0x81, 0xBB};
  std::vector<uint8_t> out(128);
  const auto status =
      brscan::DecodeRlengthRow(in.data(), in.size(), out.data(), out.size());
  ASSERT_EQ(status, brscan::Status::kOk);
  for (uint8_t b : out) EXPECT_EQ(b, 0xBB);
}

TEST(DecodeRlengthRow, NoOpControlByteProducesNoOutput) {
  // 0x02 "ABC" -> literal 3 bytes; 0x80 -> no-op (no value byte, no
  // output); 0x01 "DE" -> literal 2 bytes. Output is "ABCDE" (5 bytes)
  // from all 8 input bytes.
  const std::vector<uint8_t> in = {0x02, 'A', 'B', 'C', 0x80, 0x01, 'D', 'E'};
  std::vector<uint8_t> out(5);
  const auto status =
      brscan::DecodeRlengthRow(in.data(), in.size(), out.data(), out.size());
  ASSERT_EQ(status, brscan::Status::kOk);
  const std::vector<uint8_t> want = {'A', 'B', 'C', 'D', 'E'};
  EXPECT_EQ(out, want);
}

TEST(DecodeRlengthRow, BlankTextRow) {
  // A real blank/white row from reference/streams/modes_text_in.bin's
  // first scanline (300 dpi, area width 3472px -- see
  // reference/protocol-notes-modes.md): three repeat runs of 128 x 0x00,
  // then one repeat run of 50 x 0x00, decoding to 434 bytes
  // (ceil(3472/8)) of a solid-white 1-bit row (0 = white, see
  // PixelFormat::kBitonal in types.h). Committing this is safe: it's
  // blank/non-content, not decodable scanned material -- see
  // docs/PROTOCOL.md and PROVENANCE.md.
  const std::vector<uint8_t> in = {0x81, 0x00, 0x81, 0x00,
                                    0x81, 0x00, 0xcf, 0x00};
  std::vector<uint8_t> out(434);
  const auto status =
      brscan::DecodeRlengthRow(in.data(), in.size(), out.data(), out.size());
  ASSERT_EQ(status, brscan::Status::kOk);
  for (uint8_t b : out) EXPECT_EQ(b, 0x00);
}

TEST(DecodeRlengthRow, LiteralRunPastInputEndIsError) {
  // Control byte 0x03 claims 4 literal bytes but only 2 remain.
  const std::vector<uint8_t> in = {0x03, 0x01, 0x02};
  std::vector<uint8_t> out(4);
  const auto status =
      brscan::DecodeRlengthRow(in.data(), in.size(), out.data(), out.size());
  EXPECT_EQ(status, brscan::Status::kProtocolError);
}

TEST(DecodeRlengthRow, RepeatRunMissingValueByteIsError) {
  // Control byte 0x81 (repeat) with no following value byte.
  const std::vector<uint8_t> in = {0x81};
  std::vector<uint8_t> out(128);
  const auto status =
      brscan::DecodeRlengthRow(in.data(), in.size(), out.data(), out.size());
  EXPECT_EQ(status, brscan::Status::kProtocolError);
}

TEST(DecodeRlengthRow, OutputShorterThanDecodedIsError) {
  // The same white-line input as WhiteLine, but the caller only supplies
  // room for 300 of the 400 bytes it decodes to.
  const std::vector<uint8_t> in = {0x81, 0xFF, 0x81, 0xFF,
                                    0x81, 0xFF, 0xF1, 0xFF};
  std::vector<uint8_t> out(300);
  const auto status =
      brscan::DecodeRlengthRow(in.data(), in.size(), out.data(), out.size());
  EXPECT_EQ(status, brscan::Status::kProtocolError);
}

TEST(DecodeRlengthRow, OutputLongerThanDecodedIsError) {
  // Same input, but the caller expects more bytes than it actually
  // decodes to (a short/truncated row).
  const std::vector<uint8_t> in = {0x81, 0xFF, 0x81, 0xFF,
                                    0x81, 0xFF, 0xF1, 0xFF};
  std::vector<uint8_t> out(500);
  const auto status =
      brscan::DecodeRlengthRow(in.data(), in.size(), out.data(), out.size());
  EXPECT_EQ(status, brscan::Status::kProtocolError);
}

TEST(RlengthRowBytes, BitonalRoundsUpToWholeByte) {
  EXPECT_EQ(brscan::RlengthRowBytes(3472, /*bitonal=*/true), 434u);
  EXPECT_EQ(brscan::RlengthRowBytes(1, /*bitonal=*/true), 1u);
  EXPECT_EQ(brscan::RlengthRowBytes(8, /*bitonal=*/true), 1u);
  EXPECT_EQ(brscan::RlengthRowBytes(9, /*bitonal=*/true), 2u);
}

TEST(RlengthRowBytes, GrayIsOneBytePerPixel) {
  EXPECT_EQ(brscan::RlengthRowBytes(3472, /*bitonal=*/false), 3472u);
}

TEST(WrapBitonalImage, ExactSizeRoundTrips) {
  const int width = 9;  // 2 bytes/row.
  const int height = 3;
  std::vector<uint8_t> packed(6, 0xAA);
  brscan::Image image;
  const auto status =
      brscan::WrapBitonalImage(width, height, packed, &image);
  ASSERT_EQ(status, brscan::Status::kOk);
  EXPECT_EQ(image.width, width);
  EXPECT_EQ(image.height, height);
  EXPECT_EQ(image.format, brscan::PixelFormat::kBitonal);
  ASSERT_EQ(image.pixels.size(), 6u);
  for (uint8_t b : image.pixels) EXPECT_EQ(b, 0xAA);
}

TEST(WrapBitonalImage, WrongSizeIsError) {
  const int width = 9;
  const int height = 3;
  std::vector<uint8_t> packed(5, 0xAA);  // one byte short of 6.
  brscan::Image image;
  const auto status =
      brscan::WrapBitonalImage(width, height, packed, &image);
  EXPECT_EQ(status, brscan::Status::kProtocolError);
}
