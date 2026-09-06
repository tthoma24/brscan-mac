#include "buffer_descriptor.h"

#include <optional>

#include "brscan/scanner.h"
#include "brscan/types.h"

namespace brscan::ica {

std::optional<BufferDescriptor> DescribeBuffer(PixelFormat format, int width,
                                               int height) {
  // Reject invalid geometry before any arithmetic: a non-positive dimension has
  // no well-defined stride or byte count.
  if (width <= 0 || height <= 0) return std::nullopt;

  BufferDescriptor d;
  // Accumulate stride and size in 64-bit so a large-but-plausible page (e.g. an
  // 11x17 ledger at 600 dpi, ~6600 x 10200 px) never overflows a 32-bit int.
  const int64_t w = width;
  switch (format) {
    case PixelFormat::kRgb:
      d.bits_per_pixel = 24;
      d.samples_per_pixel = 3;
      d.bytes_per_row = w * 3;
      d.color_space = ColorSpace::kDeviceRGB;
      break;
    case PixelFormat::kGray:
      d.bits_per_pixel = 8;
      d.samples_per_pixel = 1;
      d.bytes_per_row = w;
      d.color_space = ColorSpace::kDeviceGray;
      break;
    case PixelFormat::kBitonal:
      d.bits_per_pixel = 1;
      d.samples_per_pixel = 1;
      d.bytes_per_row = (w + 7) / 8;  // Pad each row up to a whole byte.
      d.color_space = ColorSpace::kBlackWhite1Bit;
      break;
  }
  d.expected_byte_count = d.bytes_per_row * height;
  return d;
}

std::optional<BufferDescriptor> DescribeBuffer(const ScanResult& result) {
  return DescribeBuffer(result.format, result.width, result.height);
}

}  // namespace brscan::ica
