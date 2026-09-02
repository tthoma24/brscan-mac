#include "decode_jpeg.h"

#include <cstdint>
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
