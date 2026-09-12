// Tests for the pure ADF trailing-pad auto-crop (ica-module/adf_crop.h).
//
// A pure, hermetic unit: no ICADevices, no framework, no device. It counts the
// contiguous trailing rows of a decoded interleaved-RGB page that are uniform
// device padding (~128 mid-gray the scanner appends past the end of an over-sized
// ADF color sheet), so RunScanSynchronous can shorten the encoded page to the real
// sheet before writing the file. Only the row-count DECISION lives here; the JPEG
// decode and the CoreGraphics encode that consume it stay in module_main.mm.

#include "adf_crop.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace brscan::ica {
namespace {

// Appends `count` rows of `width` pixels, every pixel the solid (r,g,b) triple,
// to a tightly-packed interleaved-RGB buffer (bytesPerRow == width*3).
void AppendSolidRows(std::vector<uint8_t>* buf, int width, int count, uint8_t r,
                     uint8_t g, uint8_t b) {
  for (int i = 0; i < count; ++i) {
    for (int x = 0; x < width; ++x) {
      buf->push_back(r);
      buf->push_back(g);
      buf->push_back(b);
    }
  }
}

// Appends `count` rows of clearly non-flat "real content": a horizontal gradient
// so no row is uniform and the values sweep well outside the near-128 pad band.
void AppendContentRows(std::vector<uint8_t>* buf, int width, int count) {
  for (int i = 0; i < count; ++i) {
    for (int x = 0; x < width; ++x) {
      const uint8_t v = static_cast<uint8_t>((x * 37 + i * 11) & 0xff);
      buf->push_back(v);
      buf->push_back(static_cast<uint8_t>(v + 3));
      buf->push_back(static_cast<uint8_t>(v + 7));
    }
  }
}

constexpr int kWidth = 8;
constexpr int kStride = kWidth * 3;  // tight RGB stride

// ---------------------------------------------------------------------
// Core: N trailing flat-128 rows are counted; content below them is not.
// ---------------------------------------------------------------------

TEST(TrailingPadRows, NTrailingFlat128RowsReturnsN) {
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kWidth, 30);           // real page content on top
  AppendSolidRows(&buf, kWidth, 12, 128, 128, 128);  // device pad tail
  EXPECT_EQ(TrailingPadRows(buf.data(), kWidth, 42, kStride), 12);
}

TEST(TrailingPadRows, ContentToTheLastRowReturnsZero) {
  // A size-matched scan: no gray tail. The bottom row is real content, so nothing
  // is trimmed.
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kWidth, 40);
  EXPECT_EQ(TrailingPadRows(buf.data(), kWidth, 40, kStride), 0);
}

// ---------------------------------------------------------------------
// The flatness guard: a gray-but-NOISY tail is real content, not padding.
// ---------------------------------------------------------------------

TEST(TrailingPadRows, GrayButNoisyTailNotFlatReturnsZero) {
  // The bottom rows hover around 128 but are NOT flat: samples alternate 124/132
  // per pixel. Every sample is still inside the ±5 tolerance band [123,133], so
  // this is rejected by the FLATNESS test (row spread 8 > eps 3), not the
  // tolerance -- exactly the "128±noise, not flat" case real gray content shows.
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kWidth, 20);
  for (int i = 0; i < 10; ++i) {
    for (int x = 0; x < kWidth; ++x) {
      const uint8_t v = (x % 2 == 0) ? 124 : 132;
      buf.push_back(v);
      buf.push_back(v);
      buf.push_back(v);
    }
  }
  EXPECT_EQ(TrailingPadRows(buf.data(), kWidth, 30, kStride), 0);
}

// ---------------------------------------------------------------------
// Sparse specks: a real device pad is 128 with a thin scatter of bright
// JPEG-decode outliers, not a bit-perfect flat fill.
// ---------------------------------------------------------------------

TEST(TrailingPadRows, SparseBrightSpecksInFlat128TailStillCountsAsPad) {
  // Measured on a captured ADF color scan (C4): each gray pad row is ~98.7% of
  // samples EXACTLY 128 with ~1.3% bright specks that jump straight to >200 (up
  // to 240) -- JPEG-decode ringing, nothing between 128 and the specks. The old
  // "reject on the FIRST out-of-band sample" test dropped the crop entirely on
  // such a pad (trimmed 0 rows). A pad row must tolerate a sparse scatter of
  // far-off outliers while its in-band core stays flat.
  const int width = 100;
  const int stride = width * 3;
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, width, 20);  // real page content on top
  for (int i = 0; i < 10; ++i) {       // 10 pad rows: 128 with 2 specks (~2%)
    for (int x = 0; x < width; ++x) {
      const uint8_t v = (x == 17 || x == 63) ? 240 : 128;
      buf.push_back(v);
      buf.push_back(v);
      buf.push_back(v);
    }
  }
  EXPECT_EQ(TrailingPadRows(buf.data(), width, 30, stride), 10);
}

// ---------------------------------------------------------------------
// Full-buffer / caller-guard interaction.
// ---------------------------------------------------------------------

TEST(TrailingPadRows, FullyFlat128BufferReturnsHeightSafely) {
  // A page that is ALL pad returns every row. The helper caps nothing; the module
  // caller guards `height - pad > 0`, so this all-pad result never crops a page to
  // nothing -- here (height - pad) == 0, so the caller would leave the page whole.
  std::vector<uint8_t> buf;
  AppendSolidRows(&buf, kWidth, 25, 128, 128, 128);
  const int pad = TrailingPadRows(buf.data(), kWidth, 25, kStride);
  EXPECT_EQ(pad, 25);
  EXPECT_LE(25 - pad, 0);  // caller's `height - pad > 0` guard is false -> no crop
}

// ---------------------------------------------------------------------
// Tolerance band: a flat NON-128 margin is content; a slightly-off pad is pad.
// ---------------------------------------------------------------------

TEST(TrailingPadRows, FlatWhiteTailIsNotPadReturnsZero) {
  // A solid WHITE bottom margin (255) is flat but nowhere near 128, so the
  // tolerance band rejects it: a legitimate white margin is not a gray pad.
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kWidth, 20);
  AppendSolidRows(&buf, kWidth, 8, 255, 255, 255);
  EXPECT_EQ(TrailingPadRows(buf.data(), kWidth, 28, kStride), 0);
}

TEST(TrailingPadRows, UniformSlightlyOff128TailStillCountsAsPad) {
  // A pad whose uniform level drifted a few counts off 128 (all 131) is still
  // flat and inside the ±5 tolerance, so it is correctly counted as padding.
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kWidth, 15);
  AppendSolidRows(&buf, kWidth, 6, 131, 131, 131);
  EXPECT_EQ(TrailingPadRows(buf.data(), kWidth, 21, kStride), 6);
}

// ---------------------------------------------------------------------
// Contiguity: interior pad is never touched; the scan stops at the tail.
// ---------------------------------------------------------------------

TEST(TrailingPadRows, InteriorGrayBandNotCountedWhenContentIsBelow) {
  // A flat-128 band in the MIDDLE of the page (content above AND below it) is
  // ordinary content: the count stops at the first non-pad row from the bottom,
  // so an interior gray region is never trimmed.
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kWidth, 10);
  AppendSolidRows(&buf, kWidth, 5, 128, 128, 128);  // interior gray band
  AppendContentRows(&buf, kWidth, 10);              // real content below it
  EXPECT_EQ(TrailingPadRows(buf.data(), kWidth, 25, kStride), 0);
}

TEST(TrailingPadRows, OnlyContiguousTailIsCounted) {
  // content | pad | content | pad : only the final contiguous pad run counts.
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kWidth, 8);
  AppendSolidRows(&buf, kWidth, 4, 128, 128, 128);
  AppendContentRows(&buf, kWidth, 8);
  AppendSolidRows(&buf, kWidth, 7, 128, 128, 128);
  EXPECT_EQ(TrailingPadRows(buf.data(), kWidth, 27, kStride), 7);
}

// ---------------------------------------------------------------------
// Stride: a bytesPerRow larger than width*3 is honored; junk in the row
// padding beyond width*3 is not read.
// ---------------------------------------------------------------------

TEST(TrailingPadRows, LargerStrideHonoredJunkInPaddingIgnored) {
  const int stride = kStride + 5;  // 5 junk bytes past the pixels each row
  const int rows = 12;
  std::vector<uint8_t> buf(static_cast<size_t>(stride) * rows, 0);
  auto setRow = [&](int r, auto fill) {
    uint8_t* row = buf.data() + static_cast<size_t>(r) * stride;
    for (int x = 0; x < kWidth; ++x) fill(row + x * 3, x);
    for (int j = kStride; j < stride; ++j) row[j] = 200;  // junk in stride pad
  };
  for (int r = 0; r < 6; ++r) {
    setRow(r, [](uint8_t* p, int x) {
      const uint8_t v = static_cast<uint8_t>((x * 37) & 0xff);
      p[0] = v; p[1] = v; p[2] = v;
    });
  }
  for (int r = 6; r < rows; ++r) {  // 6 flat-128 pad rows (with junk in padding)
    setRow(r, [](uint8_t* p, int) { p[0] = 128; p[1] = 128; p[2] = 128; });
  }
  // The 200 junk bytes live only in the [width*3, stride) padding; if they were
  // read the pad rows would fail, so a count of 6 proves only width*3 is scanned.
  EXPECT_EQ(TrailingPadRows(buf.data(), kWidth, rows, stride), 6);
}

// ---------------------------------------------------------------------
// Defensive guards.
// ---------------------------------------------------------------------

TEST(TrailingPadRows, NullBufferReturnsZero) {
  EXPECT_EQ(TrailingPadRows(nullptr, kWidth, 10, kStride), 0);
}

TEST(TrailingPadRows, NonPositiveDimensionsReturnZero) {
  std::vector<uint8_t> buf(kStride, 128);
  EXPECT_EQ(TrailingPadRows(buf.data(), 0, 10, kStride), 0);
  EXPECT_EQ(TrailingPadRows(buf.data(), kWidth, 0, kStride), 0);
  EXPECT_EQ(TrailingPadRows(buf.data(), -4, 10, kStride), 0);
  EXPECT_EQ(TrailingPadRows(buf.data(), kWidth, -1, kStride), 0);
}

TEST(TrailingPadRows, StrideShorterThanRowReturnsZero) {
  std::vector<uint8_t> buf;
  AppendSolidRows(&buf, kWidth, 4, 128, 128, 128);
  EXPECT_EQ(TrailingPadRows(buf.data(), kWidth, 4, kStride - 1), 0);
}

// ---------------------------------------------------------------------
// TrailingOverscanRows: gray pad + gated near-white backing overscan.
// ---------------------------------------------------------------------

// A near-white "backing overscan" row: mostly ≥235 with almost no ink.
void AppendWhiteRows(std::vector<uint8_t>* buf, int width, int count) {
  AppendSolidRows(buf, width, count, 255, 255, 255);
}

constexpr int kOW = 100;              // width where the 97%/1% bands resolve
constexpr int kOS = kOW * 3;

TEST(TrailingOverscanRows, NoGrayPadLeavesEverythingUntouched) {
  // Trailing near-white but NO device gray pad: the device never signalled a short
  // page, so the bottom (which could be a genuine white margin) is left intact.
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kOW, 20);
  AppendWhiteRows(&buf, kOW, 30);
  EXPECT_EQ(TrailingOverscanRows(buf.data(), kOW, 50, kOS), 0);
}

TEST(TrailingOverscanRows, GrayPadWithContentAboveTrimsGrayOnly) {
  // Content sits directly on the gray pad (no near-white band between): only the
  // gray rows trim — same as TrailingPadRows.
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kOW, 20);
  AppendSolidRows(&buf, kOW, 8, 128, 128, 128);
  EXPECT_EQ(TrailingOverscanRows(buf.data(), kOW, 28, kOS), 8);
}

TEST(TrailingOverscanRows, GrayPadExtendsThroughWhiteOverscanToContent) {
  // content | near-white backing overscan | gray pad  ->  both trim, stopping at
  // the last real content row.
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kOW, 20);
  AppendWhiteRows(&buf, kOW, 15);                  // backing overscan
  AppendSolidRows(&buf, kOW, 8, 128, 128, 128);    // device gray pad
  EXPECT_EQ(TrailingOverscanRows(buf.data(), kOW, 43, kOS), 23);  // 15 + 8
}

TEST(TrailingOverscanRows, StopsAtLowerContentNotInteriorWhiteGap) {
  // content | interior white gap | content | overscan | gray. The scan from the
  // bottom stops at the LOWER content, so the interior gap and the page above it
  // are never trimmed.
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kOW, 10);
  AppendWhiteRows(&buf, kOW, 10);                  // interior white gap
  AppendContentRows(&buf, kOW, 10);               // lower content (the stop)
  AppendWhiteRows(&buf, kOW, 12);                 // trailing overscan
  AppendSolidRows(&buf, kOW, 6, 128, 128, 128);   // gray pad
  EXPECT_EQ(TrailingOverscanRows(buf.data(), kOW, 48, kOS), 18);  // 12 + 6 only
}

TEST(TrailingOverscanRows, InkyNearWhiteRowIsContentAndStopsTheScan) {
  // A near-white row carrying ink (a line of text) is content, not overscan: it
  // fails the ink test and stops the scan, so it is kept.
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kOW, 15);
  // One row: 94 white pixels + 6 black (6% ink, > kBlankMaxInkPct) — content.
  for (int x = 0; x < kOW; ++x) {
    const uint8_t v = (x < 6) ? 0 : 255;
    buf.push_back(v);
    buf.push_back(v);
    buf.push_back(v);
  }
  AppendWhiteRows(&buf, kOW, 10);                 // overscan below the text line
  AppendSolidRows(&buf, kOW, 5, 128, 128, 128);   // gray pad
  EXPECT_EQ(TrailingOverscanRows(buf.data(), kOW, 31, kOS), 15);  // 10 + 5, stop at text
}

// ---------------------------------------------------------------------
// FillTrailingRowsWhite: erase overscan in place, keep the image height.
// ---------------------------------------------------------------------

TEST(FillTrailingRowsWhite, FillsLastNRowsWhiteAndLeavesRestUntouched) {
  std::vector<uint8_t> buf;
  AppendContentRows(&buf, kOW, 20);              // 20 content rows on top
  AppendSolidRows(&buf, kOW, 10, 128, 128, 128); // 10 rows to be whitened
  FillTrailingRowsWhite(buf.data(), kOW, 30, kOS, 10);
  // Last 10 rows are now pure white.
  for (int r = 20; r < 30; ++r)
    for (int i = 0; i < kOS; ++i)
      ASSERT_EQ(buf[static_cast<size_t>(r) * kOS + i], 255) << "row " << r;
  // Row 19 (last content row) is untouched: its gradient is not all-255.
  bool row19AllWhite = true;
  for (int i = 0; i < kOS; ++i)
    if (buf[static_cast<size_t>(19) * kOS + i] != 255) row19AllWhite = false;
  EXPECT_FALSE(row19AllWhite);
}

TEST(FillTrailingRowsWhite, ZeroRowsIsNoOp) {
  std::vector<uint8_t> buf;
  AppendSolidRows(&buf, kOW, 5, 128, 128, 128);
  const std::vector<uint8_t> before = buf;
  FillTrailingRowsWhite(buf.data(), kOW, 5, kOS, 0);
  EXPECT_EQ(buf, before);
}

TEST(FillTrailingRowsWhite, RowsClampedToHeight) {
  std::vector<uint8_t> buf;
  AppendSolidRows(&buf, kOW, 4, 0, 0, 0);  // 4 black rows
  FillTrailingRowsWhite(buf.data(), kOW, 4, kOS, 999);  // asks for far too many
  for (size_t i = 0; i < buf.size(); ++i) ASSERT_EQ(buf[i], 255);  // all white, no overrun
}

TEST(FillTrailingRowsWhite, OnlyFillsWidthNotStridePadding) {
  const int stride = kOS + 5;  // 5 junk bytes past the pixels each row
  const int rows = 6;
  std::vector<uint8_t> buf(static_cast<size_t>(stride) * rows, 7);  // 7 = sentinel
  FillTrailingRowsWhite(buf.data(), kOW, rows, stride, 2);  // whiten last 2 rows
  for (int r = 4; r < rows; ++r) {
    for (int x = 0; x < kOS; ++x)
      ASSERT_EQ(buf[static_cast<size_t>(r) * stride + x], 255);  // pixels white
    for (int j = kOS; j < stride; ++j)
      ASSERT_EQ(buf[static_cast<size_t>(r) * stride + j], 7);  // stride pad untouched
  }
}

TEST(FillTrailingRowsWhite, DefensiveGuards) {
  std::vector<uint8_t> buf(kOS * 4, 0);
  FillTrailingRowsWhite(nullptr, kOW, 4, kOS, 2);          // null: no crash
  FillTrailingRowsWhite(buf.data(), 0, 4, kOS, 2);         // width 0
  FillTrailingRowsWhite(buf.data(), kOW, 0, kOS, 2);       // height 0
  FillTrailingRowsWhite(buf.data(), kOW, 4, kOS - 1, 2);   // stride too short
  for (uint8_t b : buf) EXPECT_EQ(b, 0);  // untouched by any guarded call
}

}  // namespace
}  // namespace brscan::ica
