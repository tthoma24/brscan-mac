// Objective-C++ implementation of the high-speed page rotation (see
// image_transform.h). No ARC is needed here: the decode goes through
// daemon/action_ocr.h's CreateCGImageFromScanResult and the rest is
// CoreGraphics/ImageIO/CoreFoundation opaque types (CGImageRef, CGContextRef,
// CGColorSpaceRef, CFMutableDataRef, CGImageDestinationRef), all manually
// retained/released -- the same style output_writer.mm uses for its
// CoreGraphics/ImageIO plumbing.

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

#include "action_ocr.h"
#include "image_transform.h"

namespace brscan::scand {

namespace {

// The rotation applied to a landscape-fed high-speed page to bring it back
// to portrait: 90 degrees clockwise. Negative is clockwise in CoreGraphics'
// y-up user space. This is the *single* direction knob (see
// image_transform.h): the CTM below is built around the output/input centers
// so flipping this to +M_PI_2 yields a clean 90-degree counter-clockwise
// rotation with no other change -- the fix if a device-in-the-loop test ever
// shows high-speed pages upside-down.
constexpr CGFloat kRotationRadians = -M_PI_2;

// Draws `src` (a decoded page, `in_width` x `in_height`) rotated by
// kRotationRadians into `ctx`, an already-created bitmap context sized to the
// swapped output dimensions (`in_height` x `in_width`). Interpolation and
// antialiasing are off so a 90-degree rotation is an exact pixel remap (the
// rotated source grid lands cell-for-cell on the swapped output grid), which
// is what image_transform_test.mm's exact-pixel check relies on.
void DrawRotated(CGContextRef ctx, CGImageRef src, int in_width,
                 int in_height) {
  const CGFloat out_width = static_cast<CGFloat>(in_height);
  const CGFloat out_height = static_cast<CGFloat>(in_width);

  CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
  CGContextSetShouldAntialias(ctx, false);

  // Rotate about the output center, then re-center the (differently sized)
  // input under it. With kRotationRadians = -M_PI_2 this maps the input's
  // top-left corner to the output's top-right -- a true 90-degree clockwise
  // rotation (verified by image_transform_test.mm).
  CGContextTranslateCTM(ctx, out_width / 2, out_height / 2);
  CGContextRotateCTM(ctx, kRotationRadians);
  CGContextTranslateCTM(ctx, -static_cast<CGFloat>(in_width) / 2,
                        -static_cast<CGFloat>(in_height) / 2);
  CGContextDrawImage(
      ctx, CGRectMake(0, 0, static_cast<CGFloat>(in_width),
                      static_cast<CGFloat>(in_height)),
      src);
}

// Rotates `src` into a fresh 8-bit grayscale buffer of the swapped output
// dimensions and returns it row-major, one byte per pixel (no row padding:
// bytesPerRow == out_width). Returns an empty vector if the bitmap context
// could not be created. Shared by the kGray path (used directly) and the
// kBitonal path (re-packed to 1-bpp afterwards).
std::vector<uint8_t> RotateToGray8(CGImageRef src, int in_width, int in_height,
                                   int out_width, int out_height) {
  std::vector<uint8_t> buffer(static_cast<size_t>(out_width) *
                              static_cast<size_t>(out_height));
  CGColorSpaceRef colorspace = CGColorSpaceCreateDeviceGray();
  CGContextRef ctx = CGBitmapContextCreate(
      buffer.data(), static_cast<size_t>(out_width),
      static_cast<size_t>(out_height), /*bitsPerComponent=*/8,
      /*bytesPerRow=*/static_cast<size_t>(out_width), colorspace,
      kCGBitmapByteOrderDefault | kCGImageAlphaNone);
  CGColorSpaceRelease(colorspace);
  if (ctx == nullptr) return {};

  // Clear to white first, so any output pixel a 90-degree remap somehow
  // leaves uncovered reads as paper-white rather than uninitialized.
  CGContextSetGrayFillColor(ctx, 1.0, 1.0);
  CGContextFillRect(ctx, CGRectMake(0, 0, static_cast<CGFloat>(out_width),
                                    static_cast<CGFloat>(out_height)));
  DrawRotated(ctx, src, in_width, in_height);
  CGContextRelease(ctx);
  return buffer;
}

// Re-encodes `image` as a baseline JPEG (the on-the-wire form of a kRgb
// ScanResult, decoded by CreateCGImageFromScanResult's kRgb branch). Returns
// std::nullopt on any ImageIO failure.
std::optional<std::vector<uint8_t>> EncodeJpeg(CGImageRef image) {
  CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
  if (data == nullptr) return std::nullopt;
  CGImageDestinationRef dest =
      CGImageDestinationCreateWithData(data, CFSTR("public.jpeg"), 1, nullptr);
  if (dest == nullptr) {
    CFRelease(data);
    return std::nullopt;
  }
  CGImageDestinationAddImage(dest, image, nullptr);
  const bool finalized = CGImageDestinationFinalize(dest);
  CFRelease(dest);
  if (!finalized) {
    CFRelease(data);
    return std::nullopt;
  }
  const UInt8* bytes = CFDataGetBytePtr(data);
  const CFIndex length = CFDataGetLength(data);
  std::vector<uint8_t> out(bytes, bytes + length);
  CFRelease(data);
  return out;
}

// Rotates `src` into a fresh RGBA bitmap and re-encodes it as JPEG.
std::optional<std::vector<uint8_t>> RotateRgbToJpeg(CGImageRef src,
                                                    int in_width, int in_height,
                                                    int out_width,
                                                    int out_height) {
  CGColorSpaceRef colorspace = CGColorSpaceCreateDeviceRGB();
  CGContextRef ctx = CGBitmapContextCreate(
      /*data=*/nullptr, static_cast<size_t>(out_width),
      static_cast<size_t>(out_height), /*bitsPerComponent=*/8,
      /*bytesPerRow=*/0, colorspace,
      kCGBitmapByteOrderDefault | kCGImageAlphaNoneSkipLast);
  CGColorSpaceRelease(colorspace);
  if (ctx == nullptr) return std::nullopt;

  CGContextSetRGBFillColor(ctx, 1.0, 1.0, 1.0, 1.0);
  CGContextFillRect(ctx, CGRectMake(0, 0, static_cast<CGFloat>(out_width),
                                    static_cast<CGFloat>(out_height)));
  DrawRotated(ctx, src, in_width, in_height);

  CGImageRef rotated = CGBitmapContextCreateImage(ctx);
  CGContextRelease(ctx);
  if (rotated == nullptr) return std::nullopt;
  std::optional<std::vector<uint8_t>> jpeg = EncodeJpeg(rotated);
  CGImageRelease(rotated);
  return jpeg;
}

// Packs an 8-bit grayscale raster (`gray`, out_width x out_height, row-major)
// back to kBitonal's packed 1-bit-per-pixel form: MSB-first, 1 = black, each
// row padded to a whole byte (see PixelFormat::kBitonal in
// libbrscan/include/brscan/types.h). A gray sample below 128 is treated as
// black -- the inverse of action_ocr.mm's ExpandBitonalToGray8, which mapped
// the 1=black bit to 0 and 0=white to 255.
std::vector<uint8_t> PackGray8ToBitonal(const std::vector<uint8_t>& gray,
                                        int out_width, int out_height) {
  const size_t row_bytes = (static_cast<size_t>(out_width) + 7) / 8;
  std::vector<uint8_t> packed(row_bytes * static_cast<size_t>(out_height), 0);
  for (int y = 0; y < out_height; ++y) {
    const uint8_t* in_row =
        gray.data() + static_cast<size_t>(y) * static_cast<size_t>(out_width);
    uint8_t* out_row = packed.data() + static_cast<size_t>(y) * row_bytes;
    for (int x = 0; x < out_width; ++x) {
      if (in_row[x] < 128) {
        out_row[static_cast<size_t>(x) / 8] |=
            static_cast<uint8_t>(0x80u >> (x % 8));
      }
    }
  }
  return packed;
}

}  // namespace

std::optional<brscan::ScanResult> RotatePortrait(
    const brscan::ScanResult& page) {
  if (page.width <= 0 || page.height <= 0) return std::nullopt;

  CGImageRef src = brscan::CreateCGImageFromScanResult(page);
  if (src == nullptr) {
    std::cerr << "[image_transform] could not decode page for rotation\n";
    return std::nullopt;
  }

  const int in_width = page.width;
  const int in_height = page.height;
  const int out_width = in_height;  // 90-degree rotation swaps the axes.
  const int out_height = in_width;

  brscan::ScanResult rotated;
  rotated.format = page.format;
  rotated.width = out_width;
  rotated.height = out_height;

  switch (page.format) {
    case brscan::PixelFormat::kRgb: {
      std::optional<std::vector<uint8_t>> jpeg =
          RotateRgbToJpeg(src, in_width, in_height, out_width, out_height);
      CGImageRelease(src);
      if (!jpeg) {
        std::cerr << "[image_transform] could not re-encode rotated color "
                     "page as JPEG\n";
        return std::nullopt;
      }
      rotated.data = std::move(*jpeg);
      return rotated;
    }
    case brscan::PixelFormat::kGray: {
      std::vector<uint8_t> gray =
          RotateToGray8(src, in_width, in_height, out_width, out_height);
      CGImageRelease(src);
      if (gray.empty()) {
        std::cerr << "[image_transform] could not create gray bitmap for "
                     "rotation\n";
        return std::nullopt;
      }
      rotated.data = std::move(gray);
      return rotated;
    }
    case brscan::PixelFormat::kBitonal: {
      std::vector<uint8_t> gray =
          RotateToGray8(src, in_width, in_height, out_width, out_height);
      CGImageRelease(src);
      if (gray.empty()) {
        std::cerr << "[image_transform] could not create bitmap for bitonal "
                     "rotation\n";
        return std::nullopt;
      }
      rotated.data = PackGray8ToBitonal(gray, out_width, out_height);
      return rotated;
    }
  }
  CGImageRelease(src);
  return std::nullopt;
}

}  // namespace brscan::scand
