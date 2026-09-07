// Objective-C++ implementation of host-side blank-page detection (see
// blank_detect.h). No ARC is needed here: the decode goes through
// output/action_ocr.h's CreateCGImageFromScanResult and the rest is
// CoreGraphics/CoreFoundation opaque types (CGImageRef, CGContextRef,
// CGColorSpaceRef), all manually retained/released -- the same style
// daemon/image_transform.mm uses for its CoreGraphics plumbing.

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "output/action_ocr.h"
#include "blank_detect.h"

namespace brscan::scand {

namespace {

// A pixel counts as "ink" when its 8-bit gray luminance is below this. At
// ~0.75 * 255 (191) it treats anything meaningfully darker than light-gray
// paper as content: real text/marks (black through mid-gray) register as ink,
// while paper white and faint scanner background do not. Named so it is
// tunable if a device-in-the-loop pass shows real scans reading too dark or
// too light.
constexpr double kInkLuminanceThreshold = 0.75 * 255.0;

// A page is blank when its ink coverage (fraction of ink pixels) is below
// this. At 0.5% a genuinely empty sheet -- whose only non-white pixels are
// downscaling noise and faint edge shadow -- stays under it, while even a
// single line of text clears it. Named alongside kInkLuminanceThreshold so
// the blank/non-blank boundary is tunable from one place.
constexpr double kBlankCoverageEpsilon = 0.005;

// Cap the long edge of the downscaled analysis bitmap. Blank detection needs
// no more than a thumbnail: downscaling averages out isolated speckles (so
// dust doesn't read as ink) and keeps the per-pixel scan cheap regardless of
// the page's real resolution.
constexpr int kMaxLongEdge = 512;

}  // namespace

bool IsBlankPage(CGImageRef decoded) {
  // The page's pixel dimensions come straight from the decoded image, so this
  // matches the ScanResult overload's page.width/page.height (the ScanResult's
  // native bytes always decode to an image of those dimensions).
  const int width = static_cast<int>(CGImageGetWidth(decoded));
  const int height = static_cast<int>(CGImageGetHeight(decoded));
  if (width <= 0 || height <= 0) {
    std::cerr << "[blank_detect] decoded page has no dimensions; treating as "
                 "non-blank\n";
    return false;
  }

  // Downscale so the long edge is at most kMaxLongEdge (never upscale).
  const int long_edge = std::max(width, height);
  const double scale =
      long_edge > kMaxLongEdge
          ? static_cast<double>(kMaxLongEdge) / static_cast<double>(long_edge)
          : 1.0;
  const int out_width =
      std::max(1, static_cast<int>(std::lround(width * scale)));
  const int out_height =
      std::max(1, static_cast<int>(std::lround(height * scale)));

  std::vector<uint8_t> buffer(static_cast<size_t>(out_width) *
                              static_cast<size_t>(out_height));
  CGColorSpaceRef colorspace = CGColorSpaceCreateDeviceGray();
  CGContextRef ctx = CGBitmapContextCreate(
      buffer.data(), static_cast<size_t>(out_width),
      static_cast<size_t>(out_height), /*bitsPerComponent=*/8,
      /*bytesPerRow=*/static_cast<size_t>(out_width), colorspace,
      kCGBitmapByteOrderDefault | kCGImageAlphaNone);
  CGColorSpaceRelease(colorspace);
  if (ctx == nullptr) {
    std::cerr << "[blank_detect] could not create analysis bitmap; treating "
                 "as non-blank\n";
    return false;
  }

  // Clear to white first so any output pixel the draw leaves uncovered reads
  // as paper-white (ink-free), never as uninitialized memory. Interpolation
  // stays on (the default) so downscaling averages neighboring pixels --
  // shrinking speckle noise while preserving real marks as partial ink.
  CGContextSetGrayFillColor(ctx, 1.0, 1.0);
  CGContextFillRect(ctx, CGRectMake(0, 0, static_cast<CGFloat>(out_width),
                                    static_cast<CGFloat>(out_height)));
  CGContextDrawImage(ctx,
                     CGRectMake(0, 0, static_cast<CGFloat>(out_width),
                                static_cast<CGFloat>(out_height)),
                     decoded);
  CGContextRelease(ctx);

  size_t ink = 0;
  for (const uint8_t sample : buffer) {
    if (static_cast<double>(sample) < kInkLuminanceThreshold) ++ink;
  }
  const size_t total = buffer.size();
  const double coverage =
      total == 0 ? 0.0 : static_cast<double>(ink) / static_cast<double>(total);
  return coverage < kBlankCoverageEpsilon;
}

bool IsBlankPage(const brscan::ScanResult& page) {
  if (page.width <= 0 || page.height <= 0) {
    // Nothing to inspect -> treat as non-blank (never drop a page we can't
    // measure).
    std::cerr << "[blank_detect] page has no dimensions; treating as "
                 "non-blank\n";
    return false;
  }

  // @autoreleasepool: CreateCGImageFromScanResult's kRgb (JPEG) path decodes
  // through an autoreleased NSData (see output/action_ocr.mm). The daemon has
  // no ambient pool -- its main loop is plain C++ with no NSRunLoop -- so
  // without this every color page's decode buffer would leak for the process
  // lifetime ("autoreleased with no pool in place - just leaking"). The pool
  // drains it before this returns.
  @autoreleasepool {
    CGImageRef src = brscan::CreateCGImageFromScanResult(page);
    if (src == nullptr) {
      std::cerr << "[blank_detect] could not decode page; treating as "
                   "non-blank\n";
      return false;
    }
    const bool blank = IsBlankPage(src);
    CGImageRelease(src);
    return blank;
  }
}

}  // namespace brscan::scand
