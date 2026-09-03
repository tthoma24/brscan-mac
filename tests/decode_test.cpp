#include "decode_jpeg.h"

#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>
#include <turbojpeg.h>

#include "brscan/types.h"

namespace {

// Generates a small solid-color baseline JPEG at runtime with
// libturbojpeg, so the test carries zero committed image content (see
// tests/fixtures/README.md and the Task 6 brief on not committing real
// scanned images).
std::vector<uint8_t> MakeSyntheticJpeg(int width, int height, uint8_t r,
                                        uint8_t g, uint8_t b, int quality) {
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
  for (size_t i = 0; i < rgb.size(); i += 3) {
    rgb[i] = r;
    rgb[i + 1] = g;
    rgb[i + 2] = b;
  }

  tjhandle handle = tjInitCompress();
  EXPECT_NE(handle, nullptr);

  unsigned char* jpeg_buf = nullptr;
  unsigned long jpeg_size = 0;
  const int rc = tjCompress2(handle, rgb.data(), width, /*pitch=*/0, height,
                              TJPF_RGB, &jpeg_buf, &jpeg_size, TJSAMP_444,
                              quality, TJFLAG_ACCURATEDCT);
  EXPECT_EQ(rc, 0) << tjGetErrorStr2(handle);
  tjDestroy(handle);

  std::vector<uint8_t> out(jpeg_buf, jpeg_buf + jpeg_size);
  tjFree(jpeg_buf);
  return out;
}

// A larger noise-content baseline JPEG. Random pixels compress into a
// substantial entropy-coded scan, which the warning-tolerance test needs:
// solid color compresses to almost nothing, leaving too little scan data
// to truncate into a "premature end" that still fills the pixel buffer.
// Fixed seed, so no real image content (see tests/fixtures/README.md).
std::vector<uint8_t> MakeNoiseJpeg(int width, int height, int quality) {
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto& b : rgb) b = static_cast<uint8_t>(dist(rng));

  tjhandle handle = tjInitCompress();
  EXPECT_NE(handle, nullptr);
  unsigned char* jpeg_buf = nullptr;
  unsigned long jpeg_size = 0;
  const int rc = tjCompress2(handle, rgb.data(), width, /*pitch=*/0, height,
                              TJPF_RGB, &jpeg_buf, &jpeg_size, TJSAMP_444,
                              quality, TJFLAG_ACCURATEDCT);
  EXPECT_EQ(rc, 0) << tjGetErrorStr2(handle);
  tjDestroy(handle);
  std::vector<uint8_t> out(jpeg_buf, jpeg_buf + jpeg_size);
  tjFree(jpeg_buf);
  return out;
}

// Removes the first JPEG marker segment of type `marker` (the byte after
// 0xFF), i.e. 0xFF <marker> <2-byte big-endian length> <payload>. Used to
// corrupt a valid JPEG in a specific, decoder-fatal way.
std::vector<uint8_t> EraseMarkerSegment(std::vector<uint8_t> jpeg,
                                        uint8_t marker) {
  for (size_t i = 2; i + 3 < jpeg.size(); ++i) {
    if (jpeg[i] == 0xff && jpeg[i + 1] == marker) {
      const size_t len = (static_cast<size_t>(jpeg[i + 2]) << 8) | jpeg[i + 3];
      jpeg.erase(jpeg.begin() + i, jpeg.begin() + i + 2 + len);
      break;
    }
  }
  return jpeg;
}

}  // namespace

TEST(DecodeJpeg, RoundTripsSyntheticImage) {
  const int width = 16;
  const int height = 8;
  const auto jpeg = MakeSyntheticJpeg(width, height, 200, 40, 90, 95);
  ASSERT_FALSE(jpeg.empty());

  brscan::Image image;
  const auto status = brscan::DecodeJpeg(jpeg.data(), jpeg.size(), &image);
  ASSERT_EQ(status, brscan::Status::kOk);
  EXPECT_EQ(image.width, width);
  EXPECT_EQ(image.height, height);
  EXPECT_EQ(image.format, brscan::PixelFormat::kRgb);
  ASSERT_EQ(image.pixels.size(), static_cast<size_t>(width) * height * 3);

  // Sample the center pixel; JPEG is lossy, so allow some tolerance.
  const size_t center = ((height / 2) * width + width / 2) * 3;
  EXPECT_NEAR(image.pixels[center], 200, 15);
  EXPECT_NEAR(image.pixels[center + 1], 40, 15);
  EXPECT_NEAR(image.pixels[center + 2], 90, 15);
}

TEST(DecodeJpeg, TruncatedJpegIsProtocolError) {
  const auto full_jpeg = MakeSyntheticJpeg(16, 8, 10, 20, 30, 90);
  ASSERT_GT(full_jpeg.size(), 16u);

  // SOI plus a handful of bytes: no SOF, no EOI.
  const std::vector<uint8_t> truncated(full_jpeg.begin(),
                                        full_jpeg.begin() + 8);

  brscan::Image image;
  const auto status =
      brscan::DecodeJpeg(truncated.data(), truncated.size(), &image);
  EXPECT_EQ(status, brscan::Status::kProtocolError);
}

TEST(DecodeJpeg, EmptyBufferIsProtocolError) {
  brscan::Image image;
  const auto status = brscan::DecodeJpeg(nullptr, 0, &image);
  EXPECT_EQ(status, brscan::Status::kProtocolError);
}

// A JPEG whose header parses cleanly (SOI/SOF present, dimensions
// readable) but whose actual decompression fails FATALLY: the quantization
// table the scan references has been removed, so libjpeg-turbo raises a
// fatal error_exit ("Quantization table 0x00 was not defined"), rc != 0
// with error code TJERR_FATAL. This is the case DecodeJpeg's warning
// tolerance must NOT swallow -- unlike TruncatedJpegIsProtocolError (which
// fails at the header guard, before tjDecompress2), this reaches
// tjDecompress2 and exercises the `fatal` computation, asserting a truly
// broken page is still rejected.
TEST(DecodeJpeg, ValidHeaderButFatalDecodeIsProtocolError) {
  const auto full = MakeSyntheticJpeg(32, 32, 200, 40, 90, 90);
  const auto corrupt = EraseMarkerSegment(full, 0xdb);  // 0xFFDB = DQT.
  ASSERT_LT(corrupt.size(), full.size()) << "a DQT segment must exist to drop";

  // Header still parses (SOF is untouched); only the decode is fatal.
  int w = 0, h = 0, ss = 0, cs = 0;
  tjhandle probe = tjInitDecompress();
  ASSERT_EQ(tjDecompressHeader3(probe, corrupt.data(),
                                static_cast<unsigned long>(corrupt.size()), &w,
                                &h, &ss, &cs),
            0)
      << "fixture precondition: header must still parse";
  tjDestroy(probe);

  brscan::Image image;
  const auto status = brscan::DecodeJpeg(corrupt.data(), corrupt.size(), &image);
  EXPECT_EQ(status, brscan::Status::kProtocolError);
}

// A JPEG truncated so its entropy-coded scan ends before every MCU is
// decoded: libjpeg-turbo fills the pixel buffer for the rows it has,
// reports a RECOVERABLE warning ("premature end of JPEG file", rc == -1,
// error code TJERR_WARNING), and produces a usable image. This is exactly
// what real MFC-J6920DW ADF pages do (they omit trailing blank scan
// lines), so DecodeJpeg must accept it and return the declared dimensions.
// Regression guard for decode_jpeg.cpp's warning tolerance.
TEST(DecodeJpeg, RecoverableWarningStillDecodes) {
  auto jpeg = MakeNoiseJpeg(96, 96, 90);
  ASSERT_GT(jpeg.size(), 128u);
  // Drop the trailing bytes (EOI plus a little scan data). The header and
  // the bulk of the scan remain, so the decode recovers with a warning
  // rather than failing at the header or fataling mid-scan.
  jpeg.resize(jpeg.size() - 64);

  brscan::Image image;
  const auto status = brscan::DecodeJpeg(jpeg.data(), jpeg.size(), &image);
  EXPECT_EQ(status, brscan::Status::kOk);
  EXPECT_EQ(image.width, 96);
  EXPECT_EQ(image.height, 96);
  EXPECT_EQ(image.format, brscan::PixelFormat::kRgb);
  EXPECT_EQ(image.pixels.size(), static_cast<size_t>(96) * 96 * 3);
}
