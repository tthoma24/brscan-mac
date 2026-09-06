// Plan 2 Task 4 — per-page host buffer descriptor for the ICA module.
//
// PLAN-2-DESIGN.md section G ("Data hand-back and pixel formats") recommends a
// "small descriptor unit that, given a ScanResult, produces the host's buffer
// parameters -- bits per pixel, bytes per row, color space ... This descriptor
// computation is pure and hermetically testable even though the delivery call
// into the framework is not." This file is that unit.
//
// Given only the SHAPE of one scanned page (its brscan::PixelFormat plus pixel
// width/height), it computes the parameters the ICA framework needs in order to
// receive that page's decoded, host-ready pixels: bits/samples per pixel, the
// byte stride of one row, the total decoded byte count, and a framework-free
// color-space tag.
//
// Clean-room: this is our own arithmetic over libbrscan's own public types. It
// deliberately does NOT include ICADevices or any Apple header, and its
// ColorSpace enum is a plain descriptor -- NOT a Core Graphics / ICA type -- so
// the unit stays framework-free and unit-testable. The framework-bound glue
// (Plan 2 design task 7, ICD_ScannerStart -> brscan::RunScan) translates this
// descriptor into the host's actual buffer/color-space objects.
//
// IMPORTANT -- decoded size, not payload size. The expected_byte_count this
// unit reports is the size of the DECODED, host-ready buffer computed purely
// from geometry. For PixelFormat::kRgb that is the interleaved 24-bit RGB size
// (width*3*height), NOT the length of the compressed JPEG stream carried in
// ScanResult::data -- task 7 first runs brscan::DecodeJpeg (decode_jpeg.h) to
// obtain those RGB bytes. For kGray and kBitonal the payload already IS the
// host-ready buffer and passes through unchanged, so its length matches this
// count directly. This unit never reads ScanResult::data: size comes from
// geometry alone, independent of decoded contents.
//
// IMPORTANT -- bitonal bit polarity. For PixelFormat::kBitonal this unit emits
// the libbrscan packing described in brscan/types.h: 1 bit per pixel, packed 8
// pixels to a byte, most-significant-bit first, 1 = black, each row padded to a
// whole byte (stride (width + 7) / 8). Whether the host expects that same
// polarity or an inverted one is a task-7 runtime concern (see section G,
// "confirm the host's bit-polarity expectation during the spike"); this unit
// does not guess a flip.

#pragma once

#include <cstdint>
#include <optional>

#include "brscan/scanner.h"
#include "brscan/types.h"

namespace brscan::ica {

// A framework-free color-space tag for a decoded host buffer. These are plain
// descriptors, NOT Core Graphics / ICA types; task 7 maps them onto whatever
// the framework expects. One value per libbrscan PixelFormat.
enum class ColorSpace {
  kDeviceRGB,       // kRgb: interleaved 24-bit RGB, 3 samples/pixel.
  kDeviceGray,      // kGray: 8-bit gray, 1 sample/pixel.
  kBlackWhite1Bit,  // kBitonal: packed 1-bit-per-pixel, MSB-first, 1 = black.
};

// The host buffer parameters for one decoded page, all derived from geometry.
//
// bytes_per_row and expected_byte_count are 64-bit so the stride/size
// arithmetic does not overflow for large but plausible page sizes (e.g. a
// 600-dpi ledger, ~6800 x 10200 px). expected_byte_count == bytes_per_row *
// height.
struct BufferDescriptor {
  int bits_per_pixel;
  int samples_per_pixel;
  int64_t bytes_per_row;
  int64_t expected_byte_count;
  ColorSpace color_space;
};

// Computes the host buffer descriptor for a page of `format` measured `width` x
// `height` pixels. Returns std::nullopt for invalid geometry (width <= 0 or
// height <= 0); every supported PixelFormat otherwise yields a descriptor.
//
// Mapping (per PLAN-2-DESIGN.md section G):
//   kRgb     -> 24 bpp, 3 spp, stride width*3,          kDeviceRGB
//   kGray    ->  8 bpp, 1 spp, stride width,            kDeviceGray
//   kBitonal ->  1 bpp, 1 spp, stride (width + 7) / 8,  kBlackWhite1Bit
// with expected_byte_count = stride * height in each case.
std::optional<BufferDescriptor> DescribeBuffer(PixelFormat format, int width,
                                               int height);

// Convenience overload for task-7 glue: describes the buffer for `result`'s
// page using only result.format / result.width / result.height. It does NOT
// read result.data -- the descriptor is a function of geometry alone -- so it
// is valid to call before decoding a kRgb page's JPEG payload.
std::optional<BufferDescriptor> DescribeBuffer(const ScanResult& result);

// The IMAGE-info notification arguments for ONE live band (Plan 2 Task 18b).
// A band is a contiguous run of `num_rows` rows within a page of the given
// `format` and full page dimensions, delivered by RunScan's streaming overload
// as its rows decode. The ICA module packs each band into a per-band
// kICANotificationTypeScanProgressStatus via
// ICDAddImageInfoToNotificationDictionary; these are exactly the integer
// arguments that call takes (the full page width/height, the row stride, the
// band's row offset/count, and the band byte count), computed here so the
// framework-bound glue is a straight pass-through.
struct BandImageInfo {
  int64_t width;          // Full page width in pixels.
  int64_t height;         // Full page height in pixels.
  int64_t bytes_per_row;  // Row stride for `format`/`width` (DescribeBuffer).
  int64_t data_start_row;  // Row offset of this band within the page.
  int64_t data_number_of_rows;  // Rows in this band.
  int64_t data_size;      // Band byte count == bytes_per_row * num_rows.
};

// Computes the image-info arguments for one band. This is the per-band stride
// guard the module enforces: the band's byte count MUST equal
// DescribeBuffer(format, full_width, full_height).bytes_per_row * num_rows (a
// mismatch renders nothing on the host, so it is rejected here). Returns
// std::nullopt for invalid page geometry, a non-positive band row count, a band
// row range that runs past the page height, or a band size that does not match
// the computed stride. `band_size` is the size reported by the ScanBand.
std::optional<BandImageInfo> DescribeBand(PixelFormat format, int full_width,
                                          int full_height, int start_row,
                                          int num_rows, size_t band_size);

}  // namespace brscan::ica
