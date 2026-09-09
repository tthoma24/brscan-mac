// Implementation of the pure ADF trailing-pad auto-crop (see adf_crop.h).

#include "adf_crop.h"

#include <cstddef>

namespace brscan::ica {
namespace {

// The device pads with mid-gray; the center of the accepted pad level.
constexpr int kPadValue = 128;

// A pad row's samples must each fall within ± this of kPadValue (i.e. [123,133]
// for tol=5). Wide enough to accept a pad whose uniform level drifts a few counts
// (a JPEG DC rounding, or a device pad not exactly 128); tight enough to reject a
// flat white (255) or black (0) margin of real content.
constexpr int kPadTolerance = 5;

// A pad row's IN-BAND core must be FLAT: the spread (max − min) across its
// near-128 samples must not exceed this. A truly uniform pad decodes to a
// near-constant value (only a JPEG DC term), so a spread of a couple counts is
// padding; real content — even a gray region — has larger per-pixel variation and
// is rejected. Strictly tighter than 2*kPadTolerance, which is what lets a
// noisy-but-128-ish row (e.g. dithering 124/132) be rejected even though every
// sample sits inside the tolerance band.
constexpr int kPadFlatEps = 3;

// A pad row may carry at most this percentage of samples OUTSIDE the tolerance
// band. A real device pad is not a bit-perfect fill: it shows a sparse scatter of
// bright JPEG-decode specks (measured ~1.3% of samples, jumping straight past 200
// up to 240, on a captured ADF color scan). This budget is comfortably above that
// yet far below the share a solid white/black margin (≈100% out of band) or real
// content puts out of band, so those are still rejected.
constexpr int kPadMaxOutlierPct = 5;

// True when one row of interleaved RGB is device padding (see header). A pad row
// is the pad value 128 for the overwhelming majority of its samples, tolerating a
// sparse scatter of far-off outliers (the specks above); its near-128 core must
// be flat. Rejecting a row on its FIRST out-of-band sample (as an earlier version
// did) dropped the crop entirely on a real, specked pad. `width` is guaranteed
// ≥ 1 by the caller, so there is always ≥ 3 samples.
bool RowIsPad(const uint8_t* row, int width) {
  const int samples = width * 3;
  const int outlier_budget = samples * kPadMaxOutlierPct / 100;
  int outliers = 0;
  int lo = 256;
  int hi = -1;
  for (int i = 0; i < samples; ++i) {
    const int v = row[i];
    if (v < kPadValue - kPadTolerance || v > kPadValue + kPadTolerance) {
      if (++outliers > outlier_budget) return false;  // Too many far samples.
      continue;  // A sparse speck: excluded from the in-band flatness check.
    }
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  if (hi < 0) return false;  // No in-band samples at all (e.g. a solid margin).
  return (hi - lo) <= kPadFlatEps;  // In-band core is flat → padding, not noise.
}

}  // namespace

int TrailingPadRows(const uint8_t* rgb, int width, int height, int bytesPerRow) {
  if (rgb == nullptr || width <= 0 || height <= 0) return 0;
  if (bytesPerRow < width * 3) return 0;  // Malformed stride: refuse to read.

  int pad = 0;
  for (int r = height - 1; r >= 0; --r) {
    const uint8_t* row = rgb + static_cast<std::size_t>(r) * bytesPerRow;
    if (!RowIsPad(row, width)) break;  // Pad is contiguous at the tail; stop.
    ++pad;
  }
  return pad;
}

}  // namespace brscan::ica
