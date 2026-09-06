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

std::optional<BandImageInfo> DescribeBand(PixelFormat format, int full_width,
                                          int full_height, int start_row,
                                          int num_rows, size_t band_size) {
  // The band inherits the page's stride/color-space; reuse the page descriptor
  // so a band can never disagree with the whole-page geometry.
  std::optional<BufferDescriptor> d =
      DescribeBuffer(format, full_width, full_height);
  if (!d) return std::nullopt;

  // A band must cover at least one row and stay within the page.
  if (num_rows <= 0 || start_row < 0) return std::nullopt;
  if (static_cast<int64_t>(start_row) + num_rows >
      static_cast<int64_t>(full_height)) {
    return std::nullopt;
  }

  // Stride guard: the band's byte count must be exactly stride * num_rows. A
  // silent mismatch here is what renders nothing on the host, so reject it.
  const int64_t expected = d->bytes_per_row * num_rows;
  if (static_cast<int64_t>(band_size) != expected) return std::nullopt;

  BandImageInfo info;
  info.width = full_width;
  info.height = full_height;
  info.bytes_per_row = d->bytes_per_row;
  info.data_start_row = start_row;
  info.data_number_of_rows = num_rows;
  info.data_size = expected;
  return info;
}

}  // namespace brscan::ica
