#include "brscan/scanner.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <turbojpeg.h>

#include "brscan/transport_tcp.h"
#include "brscan/types.h"
#include "command.h"
#include "fake_transport.h"

namespace {

// Builds the frame the device sends for an ESC I reply: a 1-byte status
// (0x00 in every sample seen, live or captured), a 2-byte little-endian
// length (covering the text plus its trailing NUL), the ASCII CSV text,
// then the NUL. See libbrscan/scanner.cpp's per-scan negotiate step.
std::vector<uint8_t> EncodeOfferFrame(const std::string& csv) {
  std::vector<uint8_t> out;
  out.push_back(0x00);  // status
  const uint16_t len = static_cast<uint16_t>(csv.size() + 1);
  out.push_back(static_cast<uint8_t>(len & 0xff));
  out.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
  out.insert(out.end(), csv.begin(), csv.end());
  out.push_back(0x00);
  return out;
}

// A block header with the two confirmed anchor bytes set (offset 2 =
// 0x07, offset 6 = 0x84; see ParseBlockHeader) and the given little-endian
// width in the last two bytes. `pidx` (default 1) is byte[4] --
// confirmed as the 1-based page index for a color scan (see
// reference/protocol-notes-adf-multipage.md); the other bytes are
// arbitrary filler, as their meaning is unconfirmed (see response.h).
std::vector<uint8_t> EncodeBlockHeader(uint16_t width, uint8_t pidx = 1) {
  return {0x00, 0x64, 0x07, 0x00, pidx, 0x00, 0x84, 0xc0, 0x01, 0x00, 0x00,
          static_cast<uint8_t>(width & 0xff),
          static_cast<uint8_t>((width >> 8) & 0xff)};
}

// The 12-byte block header shape the real device actually sends (anchors
// at offsets 1 and 5, i.e. EncodeBlockHeader's 13-byte shape with its
// leading 0x00 dropped; see DetectHeaderLength's doc comment in
// scanner.cpp). The 13-byte shape above is the one
// reference/streams/s0_in.bin (an older vendor-driver capture) shows;
// this one is what a live probe against the real device for this task
// found instead. `pidx` (default 1) is byte[3] -- the 1-based page index,
// the same field the 13-byte EncodeBlockHeader exposes at byte[4] (one
// later, because of that shape's leading 0x00).
std::vector<uint8_t> EncodeBlockHeader12(uint16_t width, uint8_t pidx = 1) {
  return {0x64, 0x07, 0x00, pidx, 0x00, 0x84, 0xc0, 0x01, 0x00, 0x00,
          static_cast<uint8_t>(width & 0xff),
          static_cast<uint8_t>((width >> 8) & 0xff)};
}

// A tiny solid-color baseline JPEG, generated at runtime with
// libturbojpeg so no real or committed image content is involved (see
// tests/fixtures/README.md and decode_test.cpp, which does the same).
std::vector<uint8_t> MakeSyntheticJpeg(int width, int height) {
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3, 128);
  tjhandle handle = tjInitCompress();
  unsigned char* jpeg_buf = nullptr;
  unsigned long jpeg_size = 0;
  const int rc = tjCompress2(handle, rgb.data(), width, 0, height, TJPF_RGB,
                              &jpeg_buf, &jpeg_size, TJSAMP_444, 90,
                              TJFLAG_ACCURATEDCT);
  EXPECT_EQ(rc, 0);
  tjDestroy(handle);
  std::vector<uint8_t> out(jpeg_buf, jpeg_buf + jpeg_size);
  tjFree(jpeg_buf);
  return out;
}

// A larger JPEG of deterministic pseudo-random noise, sized well past
// kMaxChunkBytes (0xfff4 = 65524) in scanner.cpp so it compresses to more
// than one network block -- solid color content compresses far too well
// to reliably clear that size at any dimensions this test can afford to
// generate. Reproducible (fixed seed), so it carries no real image
// content, per tests/fixtures/README.md.
std::vector<uint8_t> MakeLargeSyntheticJpeg(int width, int height) {
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto& b : rgb) b = static_cast<uint8_t>(dist(rng));

  tjhandle handle = tjInitCompress();
  unsigned char* jpeg_buf = nullptr;
  unsigned long jpeg_size = 0;
  const int rc = tjCompress2(handle, rgb.data(), width, 0, height, TJPF_RGB,
                              &jpeg_buf, &jpeg_size, TJSAMP_444, 95,
                              TJFLAG_ACCURATEDCT);
  EXPECT_EQ(rc, 0);
  tjDestroy(handle);
  std::vector<uint8_t> out(jpeg_buf, jpeg_buf + jpeg_size);
  tjFree(jpeg_buf);
  return out;
}

brscan::Params ColorParams() {
  brscan::Params p;
  p.mode = brscan::ScanMode::kColor;
  p.source = brscan::Source::kFlatbed;
  p.x_dpi = 100;
  p.y_dpi = 100;
  return p;
}

brscan::Params GrayParams() {
  brscan::Params p;
  p.mode = brscan::ScanMode::kGray;
  p.source = brscan::Source::kFlatbed;
  p.x_dpi = 100;
  p.y_dpi = 100;
  return p;
}

// A 12-byte block header (the shape the real device sends -- see
// EncodeBlockHeader12) with `type` at byte 0 and the little-endian
// `length` in the last two bytes, matching BlockHeader::type/width in
// response.h. Used for the RLENGTH modes (TEXT/ERRDIF/GRAY256): `type` is
// 0x42 for a PackBits-compressed row or 0x40 for an uncompressed one.
// `pidx` (default 1) is byte[3], in the same position as the color
// header's confirmed page-index byte; a per-row RLENGTH header's byte[3]
// isn't independently confirmed as a page index (docs/PROTOCOL.md's "Row
// block framing" shows it as a constant), but ParseBlockHeader ignores it
// either way, so tests may still vary it per page for fidelity to a
// multi-page capture's shape.
std::vector<uint8_t> EncodeRlengthBlockHeader(uint8_t type, uint16_t length,
                                                uint8_t pidx = 1) {
  return {type, 0x07, 0x00, pidx, 0x00, 0x84, 0x00, 0x00, 0x00, 0x00,
          static_cast<uint8_t>(length & 0xff),
          static_cast<uint8_t>((length >> 8) & 0xff)};
}

brscan::Params BlackWhiteParams() {
  brscan::Params p;
  p.mode = brscan::ScanMode::kBlackWhite;
  p.source = brscan::Source::kFlatbed;
  p.x_dpi = 300;
  p.y_dpi = 300;
  return p;
}

brscan::Params TrueGrayParams() {
  brscan::Params p;
  p.mode = brscan::ScanMode::kTrueGray;
  p.source = brscan::Source::kFlatbed;
  p.x_dpi = 300;
  p.y_dpi = 300;
  return p;
}

// Queues the part of the preamble before source selection: a ready
// greeting and an ESC Q reply (arbitrary content, drained and discarded).
void QueueConnectPreamble(brscan::FakeTransport* t) {
  t->QueueRead(std::string("+OK 200\r\n"));
  t->QueueRead(std::vector<uint8_t>{0xc1, 0x00, 0x35, 0x0a});  // ESC Q reply
  t->QueueTimeout();                                           // drain done
}

// Queues the connection preamble common to every successful flatbed scan:
// QueueConnectPreamble plus the flatbed (ESC S FB) select ack.
void QueuePreamble(brscan::FakeTransport* t) {
  QueueConnectPreamble(t);
  t->QueueRead(std::vector<uint8_t>{0x80, 0x00});              // ESC S ack
  t->QueueTimeout();                                           // drain done
}

// The 10-byte end-of-page marker that follows every page's payload:
// `82 07 00 <pidx> 00 84 00 00 00 00` (pidx is 1-based; see
// reference/protocol-notes-adf-multipage.md and docs/PROTOCOL.md's
// "Multi-page (ADF)" section). Whether a page is the job's last one is
// decided by the 2 bytes that follow this marker, not by anything inside
// it -- see EncodeJobFinalTerminator (this marker + `80 80`) below, and
// the multi-page tests, which follow it with the next page's block header
// instead.
std::vector<uint8_t> EncodeEndOfPageMarker(uint8_t pidx) {
  return {0x82, 0x07, 0x00, pidx, 0x00, 0x84, 0x00, 0x00, 0x00, 0x00};
}

// The full 12-byte job-final terminator: EncodeEndOfPageMarker(pidx)
// followed by `80 80`, meaning "no more pages." Every flatbed (and
// single-page ADF) scan ends with this as the degenerate pidx=1 case.
std::vector<uint8_t> EncodeJobFinalTerminator(uint8_t pidx) {
  std::vector<uint8_t> out = EncodeEndOfPageMarker(pidx);
  out.push_back(0x80);
  out.push_back(0x80);
  return out;
}

// The real scan-button job-final terminator: the end-of-page marker
// followed by a SINGLE 0x80, after which the device closes the connection
// (modeled by the FakeTransport queue ending). This is what the device
// actually sends in the button flow (reference/brscan-daemon-live.pcap and
// the vendor's own reference/brscan-button-options.pcap) -- NOT the driver
// flow's 0x80 0x80. Hoisted here (rather than left in the RunButtonScan
// namespace block below) so the RLENGTH/gray RunScan tests can use it too.
std::vector<uint8_t> EncodeButtonJobFinal(uint8_t pidx) {
  std::vector<uint8_t> out = EncodeEndOfPageMarker(pidx);
  out.push_back(0x80);
  return out;
}

// True if `haystack` contains `needle` as a contiguous subsequence. Used to
// assert which source-select command RunScan actually put on the wire.
bool Contains(const std::vector<uint8_t>& haystack,
              const std::vector<uint8_t>& needle) {
  if (needle.empty() || needle.size() > haystack.size()) return false;
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    if (std::equal(needle.begin(), needle.end(), haystack.begin() + i)) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(RunScan, ColorFlatbedRoundTrips) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,3460,427,5052,"));

  const auto jpeg = MakeSyntheticJpeg(16, 8);
  // The header's trailing length field is this block's own payload length
  // (see ReadChunkedJpeg in scanner.cpp): since this single tiny JPEG is
  // well under the 65524-byte chunk cap, that's just its exact size, not
  // the 0xfff4 sentinel (which would instead mean "more chunks follow").
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);
  t.QueueRead(EncodeJobFinalTerminator(1));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, ColorParams(), &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].format, brscan::PixelFormat::kRgb);
  EXPECT_EQ(pages[0].width, 16);
  EXPECT_EQ(pages[0].height, 8);
  ASSERT_EQ(pages[0].data, jpeg);
}

// A JPEG payload spanning several network blocks: every block but the
// last is exactly kMaxChunkBytes (0xfff4 = 65524) with a header declaring
// the sentinel, and the last holds the exact remainder with a header
// declaring its own exact length. See ReadChunkedJpeg in scanner.cpp for
// how this framing was found against the real device: a real ~345 KB scan
// (5 blocks under this same chunking) decoded its header fine but failed
// full JPEG decompression until the reassembly bug -- treating an
// embedded mid-stream header as image bytes -- was found and fixed.
TEST(RunScan, ColorFlatbedMultiBlockPayloadReassembles) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,3460,427,5052,"));

  const auto jpeg = MakeLargeSyntheticJpeg(300, 300);
  ASSERT_GT(jpeg.size(), 0xfff4u)
      << "test fixture must exceed one chunk to exercise reassembly";

  constexpr size_t kMaxChunkBytes = 0xfff4;
  for (size_t offset = 0; offset < jpeg.size(); offset += kMaxChunkBytes) {
    const size_t remaining = jpeg.size() - offset;
    const size_t chunk_len = std::min(remaining, kMaxChunkBytes);
    auto block = EncodeBlockHeader(static_cast<uint16_t>(chunk_len));
    block.insert(block.end(), jpeg.begin() + offset, jpeg.begin() + offset + chunk_len);
    t.QueueRead(block);
  }
  t.QueueRead(EncodeJobFinalTerminator(1));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, ColorParams(), &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].format, brscan::PixelFormat::kRgb);
  EXPECT_EQ(pages[0].width, 300);
  EXPECT_EQ(pages[0].height, 300);
  ASSERT_EQ(pages[0].data, jpeg);
}

// Same reassembly as ColorFlatbedMultiBlockPayloadReassembles, but with
// every block header in the 12-byte shape the real device actually sends
// (see EncodeBlockHeader12) instead of the legacy 13-byte shape from
// reference/streams/s0_in.bin. Before this test, only the 13-byte shape
// had hermetic coverage -- the 12-byte branch in DetectHeaderLength and
// the leading-byte-restoring normalization in ReadBlockHeader (both in
// scanner.cpp) were exercised only by manual runs against real hardware,
// so a regression there could have shipped silently.
TEST(RunScan, ColorFlatbedTwelveByteHeaderReassembles) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,3460,427,5052,"));

  const auto jpeg = MakeLargeSyntheticJpeg(300, 300);
  ASSERT_GT(jpeg.size(), 0xfff4u)
      << "test fixture must exceed one chunk to exercise reassembly";

  constexpr size_t kMaxChunkBytes = 0xfff4;
  for (size_t offset = 0; offset < jpeg.size(); offset += kMaxChunkBytes) {
    const size_t remaining = jpeg.size() - offset;
    const size_t chunk_len = std::min(remaining, kMaxChunkBytes);
    auto block = EncodeBlockHeader12(static_cast<uint16_t>(chunk_len));
    block.insert(block.end(), jpeg.begin() + offset, jpeg.begin() + offset + chunk_len);
    t.QueueRead(block);
  }
  t.QueueRead(EncodeJobFinalTerminator(1));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, ColorParams(), &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].format, brscan::PixelFormat::kRgb);
  EXPECT_EQ(pages[0].width, 300);
  EXPECT_EQ(pages[0].height, 300);
  ASSERT_EQ(pages[0].data, jpeg);
}

// Regression for the "0xfff4 is a continues-sentinel, not an exact length"
// bug (see ReadChunkedJpeg in scanner.cpp). On real ADF hardware a chunk's
// header can declare kMaxChunkBytes (0xfff4) while the chunk physically
// carries FEWER bytes -- the next block header begins before 0xfff4 bytes
// have arrived. Here chunk 0 declares 0xfff4 but emits only 40000 bytes
// before chunk 1's header. The old reader trusted 0xfff4, read 40000 real
// bytes plus 25524 bytes of chunk 1's header-and-payload as if they were
// image data, then tried to parse a block header from the middle of chunk
// 1 -- desyncing the stream into kProtocolError. The fixed reader finds
// the next header at +40000 and reads exactly that many bytes, so the JPEG
// reassembles and decodes. Uses EncodeBlockHeader12 (the 12-byte shape the
// real device sends, which the boundary scan keys on) and synthetic bytes
// only -- no captured scan content.
TEST(RunScan, ColorFlatbedShortSentinelChunkReassembles) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,3460,427,5052,"));

  const auto jpeg = MakeLargeSyntheticJpeg(300, 300);
  ASSERT_GT(jpeg.size(), 0xfff4u + 40000u)
      << "fixture must span a short sentinel chunk plus more";

  constexpr size_t kMaxChunkBytes = 0xfff4;
  constexpr size_t kShortBody = 40000;  // < kMaxChunkBytes: the anomaly.

  std::vector<uint8_t> stream;
  const auto append = [&](const std::vector<uint8_t>& b) {
    stream.insert(stream.end(), b.begin(), b.end());
  };

  // Chunk 0: header DECLARES the 0xfff4 sentinel but carries only 40000
  // bytes before the next header -- the exact real-hardware anomaly.
  append(EncodeBlockHeader12(static_cast<uint16_t>(kMaxChunkBytes)));
  stream.insert(stream.end(), jpeg.begin(), jpeg.begin() + kShortBody);

  // Remaining bytes as ordinary full 0xfff4 chunks plus an honest short
  // final chunk.
  for (size_t off = kShortBody; off < jpeg.size();) {
    const size_t remaining = jpeg.size() - off;
    const size_t chunk_len = std::min(remaining, kMaxChunkBytes);
    append(EncodeBlockHeader12(static_cast<uint16_t>(chunk_len)));
    stream.insert(stream.end(), jpeg.begin() + off, jpeg.begin() + off + chunk_len);
    off += chunk_len;
  }
  t.QueueRead(stream);
  t.QueueRead(EncodeJobFinalTerminator(1));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, ColorParams(), &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].format, brscan::PixelFormat::kRgb);
  EXPECT_EQ(pages[0].width, 300);
  EXPECT_EQ(pages[0].height, 300);
  ASSERT_EQ(pages[0].data, jpeg);
}

TEST(RunScan, GrayFlatbedRoundTrips) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  // width_px=4, height_px=3: matches the block header width below and the
  // 12-byte raw payload, since a default (all-zero) Params::area requests
  // the offer's full granted area.
  t.QueueRead(EncodeOfferFrame("300,300,2,292,4,427,3,"));

  auto payload = EncodeBlockHeader(4);
  const std::vector<uint8_t> raw(4 * 3, 0x42);
  payload.insert(payload.end(), raw.begin(), raw.end());
  t.QueueRead(payload);
  t.QueueRead(EncodeJobFinalTerminator(1));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, GrayParams(), &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].format, brscan::PixelFormat::kGray);
  EXPECT_EQ(pages[0].width, 4);
  EXPECT_EQ(pages[0].height, 3);
  EXPECT_EQ(pages[0].data, raw);
}

// --- RLENGTH modes (TEXT/ERRDIF -> kBitonal, GRAY256 -> kGray) ----------

TEST(RunScan, BlackWhiteFlatbedRoundTrips) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  // width_px=9 (row_bytes = ceil(9/8) = 2), height_px=2.
  t.QueueRead(EncodeOfferFrame("300,300,2,292,9,427,2,"));

  // Row 0: PackBits literal run of 2 bytes -> {0xAA, 0xBB}.
  auto row0 = EncodeRlengthBlockHeader(0x42, 3);
  const std::vector<uint8_t> row0_payload = {0x01, 0xAA, 0xBB};
  row0.insert(row0.end(), row0_payload.begin(), row0_payload.end());
  t.QueueRead(row0);

  // Row 1: PackBits literal run of 2 bytes -> {0xCC, 0xDD}.
  auto row1 = EncodeRlengthBlockHeader(0x42, 3);
  const std::vector<uint8_t> row1_payload = {0x01, 0xCC, 0xDD};
  row1.insert(row1.end(), row1_payload.begin(), row1_payload.end());
  t.QueueRead(row1);
  t.QueueRead(EncodeJobFinalTerminator(1));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, BlackWhiteParams(), &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].format, brscan::PixelFormat::kBitonal);
  EXPECT_EQ(pages[0].width, 9);
  EXPECT_EQ(pages[0].height, 2);
  const std::vector<uint8_t> want = {0xAA, 0xBB, 0xCC, 0xDD};
  EXPECT_EQ(pages[0].data, want);
}

// Mirrors BlackWhiteFlatbedRoundTrips but with EncodeButtonJobFinal (the
// scan-button flow's single-`0x80` job-final terminator) in place of the
// driver flow's EncodeJobFinalTerminator (`0x80 0x80`). The RLENGTH/gray
// readout loop must end the job on this single-byte terminator instead of
// blocking for a second `0x80` that never arrives -- the same bug PR #36
// fixed for RunColorScan, now in this loop (live BW/TIFF scan-button
// timeout).
TEST(RunScan, BlackWhiteFlatbedSingleByteJobFinalEndsScan) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  // width_px=9 (row_bytes = ceil(9/8) = 2), height_px=2.
  t.QueueRead(EncodeOfferFrame("300,300,2,292,9,427,2,"));

  // Row 0: PackBits literal run of 2 bytes -> {0xAA, 0xBB}.
  auto row0 = EncodeRlengthBlockHeader(0x42, 3);
  const std::vector<uint8_t> row0_payload = {0x01, 0xAA, 0xBB};
  row0.insert(row0.end(), row0_payload.begin(), row0_payload.end());
  t.QueueRead(row0);

  // Row 1: PackBits literal run of 2 bytes -> {0xCC, 0xDD}.
  auto row1 = EncodeRlengthBlockHeader(0x42, 3);
  const std::vector<uint8_t> row1_payload = {0x01, 0xCC, 0xDD};
  row1.insert(row1.end(), row1_payload.begin(), row1_payload.end());
  t.QueueRead(row1);
  t.QueueRead(EncodeButtonJobFinal(1));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, BlackWhiteParams(), &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].format, brscan::PixelFormat::kBitonal);
  EXPECT_EQ(pages[0].width, 9);
  EXPECT_EQ(pages[0].height, 2);
  const std::vector<uint8_t> want2 = {0xAA, 0xBB, 0xCC, 0xDD};
  EXPECT_EQ(pages[0].data, want2);
}

TEST(RunScan, TrueGrayFlatbedRawFallbackRowsRoundTrip) {
  // GRAY256/C=RLENGTH rows can arrive uncompressed (type 0x40) instead of
  // PackBits-compressed (type 0x42) -- confirmed the dominant shape in
  // this project's own True Gray capture (reference/streams/
  // modes_gray256_in.bin; see the issue #4 report), so RunScan must
  // accept it rather than only ever expecting type 0x42 for these modes.
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,4,427,3,"));

  const std::vector<std::vector<uint8_t>> rows = {
      {0x10, 0x11, 0x12, 0x13},
      {0x20, 0x21, 0x22, 0x23},
      {0x30, 0x31, 0x32, 0x33},
  };
  for (const auto& row : rows) {
    auto block = EncodeRlengthBlockHeader(0x40, static_cast<uint16_t>(row.size()));
    block.insert(block.end(), row.begin(), row.end());
    t.QueueRead(block);
  }
  t.QueueRead(EncodeJobFinalTerminator(1));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, TrueGrayParams(), &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].format, brscan::PixelFormat::kGray);
  EXPECT_EQ(pages[0].width, 4);
  EXPECT_EQ(pages[0].height, 3);
  std::vector<uint8_t> want;
  for (const auto& row : rows) want.insert(want.end(), row.begin(), row.end());
  EXPECT_EQ(pages[0].data, want);
}

TEST(RunScan, TrueGrayFlatbedCompressedRowsRoundTrip) {
  // Companion to TrueGrayFlatbedRawFallbackRowsRoundTrip: this project's
  // own True Gray capture happened to arrive entirely as raw (0x40) rows
  // (see the issue #4 report), so the 0x42/PackBits decode path for an
  // 8-bit-per-pixel row is only proven at the DecodeRlengthRow unit level
  // and via the 1-bit (TEXT/ERRDIF) RunScan tests above. This test drives
  // that same path end to end for GRAY256 with synthetic PackBits rows
  // (literal, repeat, and a mixed literal+repeat row) so the assembly
  // logic in ReadRlengthRows -- re-reading a block header per row -- is
  // exercised for a compressed True Gray scan too, not just raw.
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,4,427,3,"));

  // Row 0: literal run of 4 bytes -> {0x10, 0x11, 0x12, 0x13}.
  auto row0 = EncodeRlengthBlockHeader(0x42, 5);
  const std::vector<uint8_t> row0_payload = {0x03, 0x10, 0x11, 0x12, 0x13};
  row0.insert(row0.end(), row0_payload.begin(), row0_payload.end());
  t.QueueRead(row0);

  // Row 1: repeat run of 4 bytes -> {0x22, 0x22, 0x22, 0x22}.
  auto row1 = EncodeRlengthBlockHeader(0x42, 2);
  const std::vector<uint8_t> row1_payload = {0xFD, 0x22};
  row1.insert(row1.end(), row1_payload.begin(), row1_payload.end());
  t.QueueRead(row1);

  // Row 2: literal run of 2 then repeat run of 2 -> {0x30, 0x31, 0x32, 0x32}.
  auto row2 = EncodeRlengthBlockHeader(0x42, 5);
  const std::vector<uint8_t> row2_payload = {0x01, 0x30, 0x31, 0xFF, 0x32};
  row2.insert(row2.end(), row2_payload.begin(), row2_payload.end());
  t.QueueRead(row2);
  t.QueueRead(EncodeJobFinalTerminator(1));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, TrueGrayParams(), &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].format, brscan::PixelFormat::kGray);
  EXPECT_EQ(pages[0].width, 4);
  EXPECT_EQ(pages[0].height, 3);
  const std::vector<uint8_t> want = {0x10, 0x11, 0x12, 0x13,   // row 0
                                      0x22, 0x22, 0x22, 0x22,   // row 1
                                      0x30, 0x31, 0x32, 0x32};  // row 2
  EXPECT_EQ(pages[0].data, want);
}

TEST(RunScan, TruncatedRlengthPayloadIsErrorNotHang) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,9,427,2,"));

  // Header declares a 3-byte payload; only 1 byte arrives, then the queue
  // runs dry (models a device-panel cancel mid-scan).
  auto row0 = EncodeRlengthBlockHeader(0x42, 3);
  row0.push_back(0x01);
  t.QueueRead(row0);

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, BlackWhiteParams(), &pages);
  EXPECT_NE(status, brscan::Status::kOk);
  EXPECT_TRUE(pages.empty());
}

TEST(RunScan, RlengthRowDecodingToWrongWidthIsError) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  // row_bytes = ceil(9/8) = 2, but this row's payload decodes to only 1
  // byte (a control byte the device would never legitimately send for a
  // 9px-wide row): RunScan must surface this as a protocol error rather
  // than silently building a mis-sized image.
  t.QueueRead(EncodeOfferFrame("300,300,2,292,9,427,2,"));

  auto row0 = EncodeRlengthBlockHeader(0x42, 2);
  const std::vector<uint8_t> row0_payload = {0x00, 0xAA};  // literal, 1 byte.
  row0.insert(row0.end(), row0_payload.begin(), row0_payload.end());
  t.QueueRead(row0);

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, BlackWhiteParams(), &pages);
  EXPECT_EQ(status, brscan::Status::kProtocolError);
  EXPECT_TRUE(pages.empty());
}

TEST(RunScan, BusyGreetingReportsBusy) {
  brscan::FakeTransport t;
  t.QueueRead(std::string("-NG 401\r\n"));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, ColorParams(), &pages);
  EXPECT_EQ(status, brscan::Status::kBusy);
  EXPECT_TRUE(pages.empty());
}

TEST(RunScan, AdfSourceSelectNoAckReportsNoPaper) {
  brscan::FakeTransport t;
  QueueConnectPreamble(&t);
  // ESC D ADF gets no reply at all (queue goes quiet immediately): the
  // heuristic mapping for an empty feeder. See scanner.cpp's caveat on
  // this mapping.
  t.QueueTimeout();

  auto params = ColorParams();
  params.source = brscan::Source::kAdf;

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, params, &pages);
  EXPECT_EQ(status, brscan::Status::kNoPaper);
  EXPECT_TRUE(pages.empty());
}

TEST(RunScan, AdfSelectsFeederWithEscDNotFlatbed) {
  // Regression guard for the ADF source-select bug: an ADF scan must put
  // ESC D ADF on the wire and must NOT send ESC S FB, which (sent first)
  // left the device on the flatbed. See scanner.cpp's source-select block
  // and reference/streams/s0_out.bin (offsets 2044/2161).
  brscan::FakeTransport t;
  QueueConnectPreamble(&t);
  t.QueueRead(std::vector<uint8_t>{0x80, 0x00});  // ESC D ADF ack
  t.QueueTimeout();                               // drain done
  t.QueueRead(EncodeOfferFrame("300,300,2,292,3460,427,5052,"));

  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);
  t.QueueRead(EncodeJobFinalTerminator(1));

  auto params = ColorParams();
  params.source = brscan::Source::kAdf;

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, params, &pages);
  ASSERT_EQ(status, brscan::Status::kOk);

  // 0x1b 0x44 = ESC D (ADF select); 0x1b 0x53 = ESC S (flatbed select).
  EXPECT_TRUE(Contains(t.written(), {0x1b, 0x44}))
      << "ADF scan must issue ESC D ADF";
  EXPECT_FALSE(Contains(t.written(), {0x1b, 0x53}))
      << "ADF scan must not issue ESC S FB (it re-selects the flatbed)";
}

// The ESC D ADF source-select ack encodes feeder paper presence in its
// single byte: 0x80 = a document is loaded (proceed), 0xc2 = the ADF is
// empty. On an empty feeder the device otherwise proceeds and (simplex)
// falls back to scanning the flatbed glass or (duplex) sends nothing and
// times out -- both wrong. So an ADF scan whose ack is 0xc2 must return
// kNoPaper immediately, before ESC I / ESC X, with `out` cleared. The
// two byte values are protocol constants carrying no device identity
// (see PROVENANCE.md), so this synthetic test is committable.
TEST(RunScan, AdfEmptyAckReportsNoPaper) {
  brscan::FakeTransport t;
  QueueConnectPreamble(&t);
  t.QueueRead(std::vector<uint8_t>{0xc2});  // ESC D ADF ack: feeder empty.

  auto params = ColorParams();
  params.source = brscan::Source::kAdf;

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, params, &pages);
  EXPECT_EQ(status, brscan::Status::kNoPaper);
  EXPECT_TRUE(pages.empty());

  // The scan must be abandoned at the source-select ack: no ESC I offer
  // negotiate and no ESC X execute may go on the wire (which would make the
  // device fall back to the glass / hang). ESC D (0x1b 0x44) still must.
  EXPECT_TRUE(Contains(t.written(), {0x1b, 0x44}))
      << "ADF scan must still issue ESC D ADF";
  EXPECT_FALSE(Contains(t.written(), {0x1b, 0x49}))
      << "empty feeder must not reach ESC I negotiate";
  EXPECT_FALSE(Contains(t.written(), {0x1b, 0x58}))
      << "empty feeder must not reach ESC X execute";
}

// Duplex (D=DUP) empty feeder takes the same ESC D ADF ack path: a 0xc2
// ack returns kNoPaper before ESC I / ESC X, rather than sending ESC X and
// waiting out the ~24 s no-data timeout the device otherwise imposes on a
// duplex job with nothing to feed.
TEST(RunScan, AdfDuplexEmptyAckReportsNoPaper) {
  brscan::FakeTransport t;
  QueueConnectPreamble(&t);
  t.QueueRead(std::vector<uint8_t>{0xc2});  // ESC D ADF ack: feeder empty.

  auto params = ColorParams();
  params.source = brscan::Source::kAdf;
  params.duplex = true;

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, params, &pages);
  EXPECT_EQ(status, brscan::Status::kNoPaper);
  EXPECT_TRUE(pages.empty());
  EXPECT_FALSE(Contains(t.written(), {0x1b, 0x58}))
      << "empty duplex feeder must not reach ESC X execute";
}

// Companion to AdfEmptyAckReportsNoPaper: a 0x80 (document loaded) ack --
// here as the single-byte reply a live probe saw, not the 2-byte capture
// form -- must proceed through negotiate and readout to a finished scan.
// Proves the new ack check gates only on 0xc2 and leaves the loaded path
// (including a 1-byte ack) fully intact.
TEST(RunScan, AdfLoadedAckProceedsToScan) {
  brscan::FakeTransport t;
  QueueConnectPreamble(&t);
  t.QueueRead(std::vector<uint8_t>{0x80});  // ESC D ADF ack: document loaded.
  t.QueueTimeout();                         // drain done
  t.QueueRead(EncodeOfferFrame("300,300,1,292,3460,0,0,"));

  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);
  t.QueueRead(EncodeJobFinalTerminator(1));

  auto params = ColorParams();
  params.source = brscan::Source::kAdf;
  params.area = brscan::Area{0, 0, 16, 8};

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, params, &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].data, jpeg);
  EXPECT_TRUE(Contains(t.written(), {0x1b, 0x58}))
      << "loaded feeder must reach ESC X execute";
}

TEST(RunScan, TruncatedColorPayloadIsErrorNotHang) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,3460,427,5052,"));

  // A block header followed by JPEG-looking bytes with no EOI marker, and
  // then nothing more: models a device-panel cancel mid-scan
  // (docs/PROTOCOL.md, "Cancellation" -- no explicit status, the stream
  // just stops). The queue then runs dry, so the read loop must surface
  // an error instead of hanging or crashing.
  auto payload = EncodeBlockHeader(0xfff4);
  const std::vector<uint8_t> partial_jpeg = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10};
  payload.insert(payload.end(), partial_jpeg.begin(), partial_jpeg.end());
  t.QueueRead(payload);

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, ColorParams(), &pages);
  EXPECT_NE(status, brscan::Status::kOk);
  EXPECT_TRUE(pages.empty());
}

TEST(RunScan, TruncatedGrayPayloadIsErrorNotHang) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,4,427,3,"));

  // width*height = 12 bytes expected; only 5 arrive, then the queue runs
  // dry.
  auto payload = EncodeBlockHeader(4);
  const std::vector<uint8_t> short_raw(5, 0x42);
  payload.insert(payload.end(), short_raw.begin(), short_raw.end());
  t.QueueRead(payload);

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, GrayParams(), &pages);
  EXPECT_NE(status, brscan::Status::kOk);
  EXPECT_TRUE(pages.empty());
}

// --- Multi-page (ADF) -----------------------------------------------------

// A 3-page synthetic ADF color scan: each page is its own independent
// baseline JPEG, separated by the 10-byte end-of-page marker (see
// EncodeEndOfPageMarker) whose trailing 2 bytes -- the next page's `64 07`
// block header -- tell RunScan's page loop to keep going rather than stop.
// Mirrors reference/protocol-notes-adf-multipage.md's decoded framing.
TEST(RunScan, ColorAdfMultiPageReturnsAllPages) {
  brscan::FakeTransport t;
  QueueConnectPreamble(&t);
  t.QueueRead(std::vector<uint8_t>{0x80, 0x00});  // ESC D ADF ack
  t.QueueTimeout();                               // drain done
  // The ADF tell in a real offer is flag=1, ymax=0 (the feeder can't know
  // page length up front; see protocol-notes-adf-multipage.md). RunScan's
  // color path never reads exec_params.area, so an explicit area below
  // sidesteps ymax=0 rather than exercising the "request the offer's full
  // area" branch, which isn't this test's concern.
  t.QueueRead(EncodeOfferFrame("300,300,1,292,3460,0,0,"));

  auto params = ColorParams();
  params.source = brscan::Source::kAdf;
  params.area = brscan::Area{0, 0, 16, 8};

  const auto jpeg1 = MakeSyntheticJpeg(16, 8);
  const auto jpeg2 = MakeSyntheticJpeg(16, 8);
  const auto jpeg3 = MakeSyntheticJpeg(16, 8);

  auto block1 = EncodeBlockHeader(static_cast<uint16_t>(jpeg1.size()), 1);
  block1.insert(block1.end(), jpeg1.begin(), jpeg1.end());
  t.QueueRead(block1);
  t.QueueRead(EncodeEndOfPageMarker(1));  // tail (next 2 bytes): `64 07`.

  auto block2 = EncodeBlockHeader(static_cast<uint16_t>(jpeg2.size()), 2);
  block2.insert(block2.end(), jpeg2.begin(), jpeg2.end());
  t.QueueRead(block2);
  t.QueueRead(EncodeEndOfPageMarker(2));  // tail (next 2 bytes): `64 07`.

  auto block3 = EncodeBlockHeader(static_cast<uint16_t>(jpeg3.size()), 3);
  block3.insert(block3.end(), jpeg3.begin(), jpeg3.end());
  t.QueueRead(block3);
  t.QueueRead(EncodeJobFinalTerminator(3));  // tail: `80 80`, job done.

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, params, &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 3u);
  for (const auto& page : pages) {
    EXPECT_EQ(page.format, brscan::PixelFormat::kRgb);
    EXPECT_EQ(page.width, 16);
    EXPECT_EQ(page.height, 8);
  }
  EXPECT_EQ(pages[0].data, jpeg1);
  EXPECT_EQ(pages[1].data, jpeg2);
  EXPECT_EQ(pages[2].data, jpeg3);
}

// Exercises ReadChunkedJpeg's sentinel-boundary-is-the-end-of-page-marker
// branch (the `is_end_of_page` break in scanner.cpp): each page's single
// chunk header DECLARES the 0xfff4 sentinel, yet its data ends right at the
// 10-byte end-of-page marker (the boundary is the MARKER, not a next block
// header). ReadChunkedJpeg must read exactly up to the marker, leave the
// marker unconsumed, and return -- so RunScan's own ReadExact(10) then
// consumes it and finds the next page. The existing multi-page tests only
// end a page on a sub-0xfff4 honest final chunk or a next-header boundary,
// so this marker-boundary branch (and the "marker left intact" contract)
// is otherwise uncovered. Two pages prove page 2 is still framed correctly
// after page 1 left its marker for the loop.
TEST(RunScan, ColorAdfSentinelChunkEndingAtMarkerReturnsAllPages) {
  brscan::FakeTransport t;
  QueueConnectPreamble(&t);
  t.QueueRead(std::vector<uint8_t>{0x80, 0x00});  // ESC D ADF ack
  t.QueueTimeout();                               // drain done
  t.QueueRead(EncodeOfferFrame("300,300,1,292,3460,0,0,"));

  auto params = ColorParams();
  params.source = brscan::Source::kAdf;
  params.area = brscan::Area{0, 0, 16, 8};

  const auto jpeg1 = MakeSyntheticJpeg(16, 8);
  const auto jpeg2 = MakeSyntheticJpeg(16, 8);
  ASSERT_LT(jpeg1.size(), 0xfff4u);
  ASSERT_LT(jpeg2.size(), 0xfff4u);

  // Header declares the 0xfff4 sentinel (12-byte shape) though the payload
  // is far shorter and is immediately followed by the end-of-page marker.
  // Each page's header pidx matches its end-of-page marker so the
  // de-interleaving reader routes and finalizes each page correctly.
  auto block1 = EncodeBlockHeader12(static_cast<uint16_t>(0xfff4), 1);
  block1.insert(block1.end(), jpeg1.begin(), jpeg1.end());
  t.QueueRead(block1);
  t.QueueRead(EncodeEndOfPageMarker(1));  // tail after marker: `64 07`.

  auto block2 = EncodeBlockHeader12(static_cast<uint16_t>(0xfff4), 2);
  block2.insert(block2.end(), jpeg2.begin(), jpeg2.end());
  t.QueueRead(block2);
  t.QueueRead(EncodeJobFinalTerminator(2));  // tail: `80 80`, job done.

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, params, &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 2u);
  for (const auto& page : pages) {
    EXPECT_EQ(page.format, brscan::PixelFormat::kRgb);
    EXPECT_EQ(page.width, 16);
    EXPECT_EQ(page.height, 8);
  }
  EXPECT_EQ(pages[0].data, jpeg1);
  EXPECT_EQ(pages[1].data, jpeg2);
}

// A 2-page synthetic DUPLEX color ADF scan whose two pages' chunks are
// INTERLEAVED on the wire, each block header tagged with its 1-based page
// index (pidx) and each page closed by its own end-of-page marker carrying
// that index -- the framing a real duplex feed uses (see
// reference/protocol-notes-adf-multipage.md and docs/PROTOCOL.md's
// "Multi-page (ADF)" duplex note). The chunk order here is p1a, p2a, p1b,
// EOP(1), p2b, EOP(2)+`80 80`: page 1 completes and its marker arrives while
// page 2 is still mid-stream, so this proves the reader keeps a separate
// per-page accumulator (page 2's bytes survive page 1's finalization) and
// de-interleaves by pidx rather than assuming one whole page at a time. The
// pages have different dimensions so a mis-routed chunk would fail the
// per-page dimension/byte-exact assertions. Synthetic JPEGs only -- no
// captured scan content (the real interleaved capture stays out of the tree;
// see PROVENANCE.md).
TEST(RunScan, ColorAdfDuplexInterleavedPagesDeinterleave) {
  brscan::FakeTransport t;
  QueueConnectPreamble(&t);
  t.QueueRead(std::vector<uint8_t>{0x80, 0x00});  // ESC D ADF ack
  t.QueueTimeout();                               // drain done
  t.QueueRead(EncodeOfferFrame("300,300,1,292,3460,0,0,"));

  auto params = ColorParams();
  params.source = brscan::Source::kAdf;
  params.duplex = true;
  params.area = brscan::Area{0, 0, 24, 12};

  // Distinct dimensions so a chunk routed to the wrong page is caught.
  const auto jpeg1 = MakeSyntheticJpeg(16, 8);
  const auto jpeg2 = MakeSyntheticJpeg(24, 12);
  ASSERT_GT(jpeg1.size(), 4u);
  ASSERT_GT(jpeg2.size(), 4u);

  // Split each page's JPEG into two chunks so each page genuinely spans more
  // than one interleaved block.
  const size_t split1 = jpeg1.size() / 2;
  const size_t split2 = jpeg2.size() / 2;

  const auto chunk = [](const std::vector<uint8_t>& jpeg, size_t begin,
                        size_t end, uint8_t pidx) {
    auto block = EncodeBlockHeader(static_cast<uint16_t>(end - begin), pidx);
    block.insert(block.end(), jpeg.begin() + begin, jpeg.begin() + end);
    return block;
  };

  std::vector<uint8_t> stream;
  const auto append = [&](const std::vector<uint8_t>& b) {
    stream.insert(stream.end(), b.begin(), b.end());
  };
  append(chunk(jpeg1, 0, split1, 1));               // page 1, chunk A.
  append(chunk(jpeg2, 0, split2, 2));               // page 2, chunk A.
  append(chunk(jpeg1, split1, jpeg1.size(), 1));    // page 1, chunk B (done).
  append(EncodeEndOfPageMarker(1));                 // finalize page 1.
  append(chunk(jpeg2, split2, jpeg2.size(), 2));    // page 2, chunk B (done).
  append(EncodeJobFinalTerminator(2));              // finalize page 2, `80 80`.
  t.QueueRead(stream);

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, params, &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 2u);
  // Pages come out in end-of-page (page) order: 1 then 2.
  EXPECT_EQ(pages[0].format, brscan::PixelFormat::kRgb);
  EXPECT_EQ(pages[0].width, 16);
  EXPECT_EQ(pages[0].height, 8);
  EXPECT_EQ(pages[0].data, jpeg1);
  EXPECT_EQ(pages[1].format, brscan::PixelFormat::kRgb);
  EXPECT_EQ(pages[1].width, 24);
  EXPECT_EQ(pages[1].height, 12);
  EXPECT_EQ(pages[1].data, jpeg2);
}

// A 2-page synthetic ADF gray (GRAY64/C=NONE) scan: each page is a raw,
// unchunked payload (see ReadRawGray), separated by the same 10-byte
// marker as the color case -- the marker framing is payload-type-
// independent (docs/PROTOCOL.md, "Multi-page (ADF)"). Gray/RLENGTH
// multi-page ADF framing is not in this project's own capture (only
// color/JPEG is -- see PROVENANCE.md), so this exercises the loop against
// a synthetic stream built to the same documented framing, not a
// hardware-confirmed one.
TEST(RunScan, GrayAdfMultiPageReturnsAllPages) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,4,427,3,"));

  auto block1 = EncodeBlockHeader(4, 1);
  const std::vector<uint8_t> raw1(4 * 3, 0x11);
  block1.insert(block1.end(), raw1.begin(), raw1.end());
  t.QueueRead(block1);
  t.QueueRead(EncodeEndOfPageMarker(1));

  auto block2 = EncodeBlockHeader(4, 2);
  const std::vector<uint8_t> raw2(4 * 3, 0x22);
  block2.insert(block2.end(), raw2.begin(), raw2.end());
  t.QueueRead(block2);
  t.QueueRead(EncodeJobFinalTerminator(2));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, GrayParams(), &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 2u);
  for (const auto& page : pages) {
    EXPECT_EQ(page.format, brscan::PixelFormat::kGray);
    EXPECT_EQ(page.width, 4);
    EXPECT_EQ(page.height, 3);
  }
  EXPECT_EQ(pages[0].data, raw1);
  EXPECT_EQ(pages[1].data, raw2);
}

// A 2-page synthetic ADF Black & White (TEXT/C=RLENGTH) scan: each page's
// rows are read the same way as the single-page case
// (BlackWhiteFlatbedRoundTrips), with the 10-byte marker between the last
// row of page 1 and the first row header of page 2. Same capture caveat as
// GrayAdfMultiPageReturnsAllPages: synthetic, per the documented framing,
// not a hardware-confirmed RLENGTH multi-page capture.
TEST(RunScan, BlackWhiteAdfMultiPageReturnsAllPages) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,9,427,2,"));

  // Page 1: rows {0xAA, 0xBB} and {0xCC, 0xDD}, each a 2-byte literal run.
  auto p1row0 = EncodeRlengthBlockHeader(0x42, 3, 1);
  const std::vector<uint8_t> p1row0_payload = {0x01, 0xAA, 0xBB};
  p1row0.insert(p1row0.end(), p1row0_payload.begin(), p1row0_payload.end());
  t.QueueRead(p1row0);

  auto p1row1 = EncodeRlengthBlockHeader(0x42, 3, 1);
  const std::vector<uint8_t> p1row1_payload = {0x01, 0xCC, 0xDD};
  p1row1.insert(p1row1.end(), p1row1_payload.begin(), p1row1_payload.end());
  t.QueueRead(p1row1);
  t.QueueRead(EncodeEndOfPageMarker(1));

  // Page 2: rows {0x11, 0x22} and {0x33, 0x44}.
  auto p2row0 = EncodeRlengthBlockHeader(0x42, 3, 2);
  const std::vector<uint8_t> p2row0_payload = {0x01, 0x11, 0x22};
  p2row0.insert(p2row0.end(), p2row0_payload.begin(), p2row0_payload.end());
  t.QueueRead(p2row0);

  auto p2row1 = EncodeRlengthBlockHeader(0x42, 3, 2);
  const std::vector<uint8_t> p2row1_payload = {0x01, 0x33, 0x44};
  p2row1.insert(p2row1.end(), p2row1_payload.begin(), p2row1_payload.end());
  t.QueueRead(p2row1);
  t.QueueRead(EncodeJobFinalTerminator(2));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, BlackWhiteParams(), &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 2u);
  for (const auto& page : pages) {
    EXPECT_EQ(page.format, brscan::PixelFormat::kBitonal);
    EXPECT_EQ(page.width, 9);
    EXPECT_EQ(page.height, 2);
  }
  const std::vector<uint8_t> want1 = {0xAA, 0xBB, 0xCC, 0xDD};
  const std::vector<uint8_t> want2 = {0x11, 0x22, 0x33, 0x44};
  EXPECT_EQ(pages[0].data, want1);
  EXPECT_EQ(pages[1].data, want2);
}

// Mirrors BlackWhiteAdfMultiPageReturnsAllPages but the job's last page ends
// with EncodeButtonJobFinal (single `0x80`) instead of
// EncodeJobFinalTerminator (`0x80 0x80`), for parity with the color path's
// MultiPageSingleByteJobFinalReturnsAllPages test.
TEST(RunScan, BlackWhiteAdfMultiPageSingleByteJobFinalReturnsAllPages) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,9,427,2,"));

  // Page 1: rows {0xAA, 0xBB} and {0xCC, 0xDD}, each a 2-byte literal run.
  auto p1row0 = EncodeRlengthBlockHeader(0x42, 3, 1);
  const std::vector<uint8_t> p1row0_payload = {0x01, 0xAA, 0xBB};
  p1row0.insert(p1row0.end(), p1row0_payload.begin(), p1row0_payload.end());
  t.QueueRead(p1row0);

  auto p1row1 = EncodeRlengthBlockHeader(0x42, 3, 1);
  const std::vector<uint8_t> p1row1_payload = {0x01, 0xCC, 0xDD};
  p1row1.insert(p1row1.end(), p1row1_payload.begin(), p1row1_payload.end());
  t.QueueRead(p1row1);
  t.QueueRead(EncodeEndOfPageMarker(1));

  // Page 2: rows {0x11, 0x22} and {0x33, 0x44}.
  auto p2row0 = EncodeRlengthBlockHeader(0x42, 3, 2);
  const std::vector<uint8_t> p2row0_payload = {0x01, 0x11, 0x22};
  p2row0.insert(p2row0.end(), p2row0_payload.begin(), p2row0_payload.end());
  t.QueueRead(p2row0);

  auto p2row1 = EncodeRlengthBlockHeader(0x42, 3, 2);
  const std::vector<uint8_t> p2row1_payload = {0x01, 0x33, 0x44};
  p2row1.insert(p2row1.end(), p2row1_payload.begin(), p2row1_payload.end());
  t.QueueRead(p2row1);
  t.QueueRead(EncodeButtonJobFinal(2));

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, BlackWhiteParams(), &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 2u);
  for (const auto& page : pages) {
    EXPECT_EQ(page.format, brscan::PixelFormat::kBitonal);
    EXPECT_EQ(page.width, 9);
    EXPECT_EQ(page.height, 2);
  }
  const std::vector<uint8_t> want3 = {0xAA, 0xBB, 0xCC, 0xDD};
  const std::vector<uint8_t> want4 = {0x11, 0x22, 0x33, 0x44};
  EXPECT_EQ(pages[0].data, want3);
  EXPECT_EQ(pages[1].data, want4);
}

// --- Streaming RunScan (per-band callback) --------------------------------

namespace {

// A band captured by the streaming RunScan callback. ScanBand::data is only
// valid during the callback, so the bytes are copied out here.
struct CapturedBand {
  int page_index;
  brscan::PixelFormat format;
  int full_width;
  int full_height;
  int start_row;
  int num_rows;
  std::vector<uint8_t> data;
};

size_t BandStride(brscan::PixelFormat f, int width) {
  switch (f) {
    case brscan::PixelFormat::kRgb:
      return static_cast<size_t>(width) * 3;
    case brscan::PixelFormat::kGray:
      return static_cast<size_t>(width);
    case brscan::PixelFormat::kBitonal:
      return (static_cast<size_t>(width) + 7) / 8;
  }
  return 0;
}

// A BandCallback that copies every band into `sink`.
brscan::BandCallback Collect(std::vector<CapturedBand>* sink) {
  return [sink](const brscan::ScanBand& b) {
    CapturedBand c;
    c.page_index = b.page_index;
    c.format = b.format;
    c.full_width = b.full_width;
    c.full_height = b.full_height;
    c.start_row = b.start_row;
    c.num_rows = b.num_rows;
    c.data.assign(b.data, b.data + b.size);
    sink->push_back(std::move(c));
    return true;
  };
}

// Decodes a whole JPEG buffer to interleaved RGB exactly as DecodeJpeg does
// (TJPF_RGB, accurate IDCT) -- the byte-exact oracle the streaming color
// bands must reconstruct.
std::vector<uint8_t> DecodeWholeJpegRgb(const std::vector<uint8_t>& jpeg) {
  tjhandle h = tjInitDecompress();
  int w = 0, hgt = 0, ss = 0, cs = 0;
  EXPECT_EQ(tjDecompressHeader3(h, jpeg.data(),
                                static_cast<unsigned long>(jpeg.size()), &w,
                                &hgt, &ss, &cs),
            0);
  std::vector<uint8_t> rgb(static_cast<size_t>(w) * hgt * 3);
  EXPECT_EQ(tjDecompress2(h, jpeg.data(),
                          static_cast<unsigned long>(jpeg.size()), rgb.data(),
                          w, 0, hgt, TJPF_RGB, TJFLAG_ACCURATEDCT),
            0);
  tjDestroy(h);
  return rgb;
}

// Groups captured bands by page_index, verifies per page that the bands are
// contiguous, monotonic, and cover [0, full_height) exactly once, and
// returns the concatenated (row-order) band pixels per page_index.
std::map<int, std::vector<uint8_t>> ConcatBandsPerPage(
    const std::vector<CapturedBand>& bands) {
  std::map<int, std::vector<uint8_t>> pixels;
  std::map<int, int> next_row;  // Expected start_row of the next band.
  std::map<int, int> height;    // full_height reported for the page.
  for (const auto& b : bands) {
    EXPECT_EQ(b.start_row, next_row[b.page_index])
        << "bands must be contiguous with no gap or overlap";
    EXPECT_GT(b.num_rows, 0);
    EXPECT_EQ(b.data.size(),
              static_cast<size_t>(b.num_rows) *
                  BandStride(b.format, b.full_width));
    next_row[b.page_index] = b.start_row + b.num_rows;
    height[b.page_index] = b.full_height;
    pixels[b.page_index].insert(pixels[b.page_index].end(), b.data.begin(),
                                b.data.end());
  }
  for (const auto& kv : height) {
    EXPECT_EQ(next_row[kv.first], kv.second)
        << "bands must cover every row of the page exactly once";
  }
  return pixels;
}

}  // namespace

// Core invariant, color: the concatenation of the streaming bands (in
// page/row order) is byte-identical to the whole-page image the
// non-streaming path decodes. The JPEG spans several network blocks so the
// suspending incremental decoder is fed across multiple chunks, exactly as
// on the wire.
TEST(RunScan, StreamingColorFlatbedBandsMatchWholePage) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,3460,427,5052,"));

  const auto jpeg = MakeLargeSyntheticJpeg(300, 300);
  ASSERT_GT(jpeg.size(), 0xfff4u);
  constexpr size_t kMaxChunkBytes = 0xfff4;
  for (size_t offset = 0; offset < jpeg.size(); offset += kMaxChunkBytes) {
    const size_t remaining = jpeg.size() - offset;
    const size_t chunk_len = std::min(remaining, kMaxChunkBytes);
    auto block = EncodeBlockHeader(static_cast<uint16_t>(chunk_len));
    block.insert(block.end(), jpeg.begin() + offset,
                 jpeg.begin() + offset + chunk_len);
    t.QueueRead(block);
  }
  t.QueueRead(EncodeJobFinalTerminator(1));

  std::vector<CapturedBand> bands;
  std::vector<brscan::ScanResult> pages;
  const auto status =
      brscan::RunScan(t, ColorParams(), &pages, Collect(&bands));
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].width, 300);
  EXPECT_EQ(pages[0].height, 300);
  ASSERT_EQ(pages[0].data, jpeg);  // Non-streaming payload still delivered.

  ASSERT_FALSE(bands.empty());
  for (const auto& b : bands) {
    EXPECT_EQ(b.page_index, 0);
    EXPECT_EQ(b.format, brscan::PixelFormat::kRgb);
    EXPECT_EQ(b.full_width, 300);
    EXPECT_EQ(b.full_height, 300);
  }
  const auto per_page = ConcatBandsPerPage(bands);
  ASSERT_EQ(per_page.size(), 1u);
  EXPECT_EQ(per_page.at(0), DecodeWholeJpegRgb(jpeg));
}

// Same invariant across a multi-page (simplex ADF) color scan: each page's
// bands reconstruct that page's whole decoded image.
TEST(RunScan, StreamingColorAdfMultiPageBandsMatchWholePages) {
  brscan::FakeTransport t;
  QueueConnectPreamble(&t);
  t.QueueRead(std::vector<uint8_t>{0x80, 0x00});  // ESC D ADF ack
  t.QueueTimeout();                               // drain done
  t.QueueRead(EncodeOfferFrame("300,300,1,292,3460,0,0,"));

  auto params = ColorParams();
  params.source = brscan::Source::kAdf;
  params.area = brscan::Area{0, 0, 300, 300};

  const auto jpeg1 = MakeLargeSyntheticJpeg(300, 300);
  const auto jpeg2 = MakeLargeSyntheticJpeg(300, 300);
  ASSERT_GT(jpeg1.size(), 0xfff4u);

  const auto emit_page = [&](const std::vector<uint8_t>& jpeg, uint8_t pidx) {
    constexpr size_t kMaxChunkBytes = 0xfff4;
    for (size_t offset = 0; offset < jpeg.size(); offset += kMaxChunkBytes) {
      const size_t remaining = jpeg.size() - offset;
      const size_t chunk_len = std::min(remaining, kMaxChunkBytes);
      auto block = EncodeBlockHeader(static_cast<uint16_t>(chunk_len), pidx);
      block.insert(block.end(), jpeg.begin() + offset,
                   jpeg.begin() + offset + chunk_len);
      t.QueueRead(block);
    }
  };
  emit_page(jpeg1, 1);
  t.QueueRead(EncodeEndOfPageMarker(1));  // tail: next `64 07` header.
  emit_page(jpeg2, 2);
  t.QueueRead(EncodeJobFinalTerminator(2));

  std::vector<CapturedBand> bands;
  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, params, &pages, Collect(&bands));
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 2u);

  const auto per_page = ConcatBandsPerPage(bands);
  ASSERT_EQ(per_page.size(), 2u);
  EXPECT_EQ(per_page.at(0), DecodeWholeJpegRgb(jpeg1));
  EXPECT_EQ(per_page.at(1), DecodeWholeJpegRgb(jpeg2));
}

// Duplex: two pages' chunks interleaved on the wire, each tagged with its
// page index. The per-page suspending decoders must keep separate state so
// each page's bands still reconstruct its own whole image.
TEST(RunScan, StreamingColorDuplexInterleavedBandsMatchWholePages) {
  brscan::FakeTransport t;
  QueueConnectPreamble(&t);
  t.QueueRead(std::vector<uint8_t>{0x80, 0x00});  // ESC D ADF ack
  t.QueueTimeout();                               // drain done
  t.QueueRead(EncodeOfferFrame("300,300,1,292,3460,0,0,"));

  auto params = ColorParams();
  params.source = brscan::Source::kAdf;
  params.duplex = true;
  params.area = brscan::Area{0, 0, 300, 300};

  const auto jpeg1 = MakeLargeSyntheticJpeg(300, 300);
  const auto jpeg2 = MakeLargeSyntheticJpeg(320, 288);
  ASSERT_GT(jpeg1.size(), 0xfff4u);
  ASSERT_GT(jpeg2.size(), 0xfff4u);

  // Split a page's JPEG into on-the-wire blocks whose lengths fit the 16-bit
  // block-header width field (and stay under the 0xfff4 sentinel).
  const auto blocks = [](const std::vector<uint8_t>& jpeg, uint8_t pidx) {
    constexpr size_t kChunk = 60000;
    std::vector<std::vector<uint8_t>> out;
    for (size_t off = 0; off < jpeg.size(); off += kChunk) {
      const size_t len = std::min(kChunk, jpeg.size() - off);
      auto block = EncodeBlockHeader(static_cast<uint16_t>(len), pidx);
      block.insert(block.end(), jpeg.begin() + off, jpeg.begin() + off + len);
      out.push_back(std::move(block));
    }
    return out;
  };
  const auto b1 = blocks(jpeg1, 1);
  const auto b2 = blocks(jpeg2, 2);
  ASSERT_GT(b1.size(), 1u);
  ASSERT_GT(b2.size(), 1u);

  std::vector<uint8_t> stream;
  const auto append = [&](const std::vector<uint8_t>& b) {
    stream.insert(stream.end(), b.begin(), b.end());
  };
  // Interleave: page1 chunk 0, page2 chunk 0, then page1's remaining chunks,
  // then EOP(1) -- page 1 completes while page 2 is still mid-stream -- then
  // page2's remaining chunks and EOP(2). This forces the per-page decoders to
  // keep independent state across page 1's finalization.
  append(b1[0]);
  append(b2[0]);
  for (size_t i = 1; i < b1.size(); ++i) append(b1[i]);
  append(EncodeEndOfPageMarker(1));
  for (size_t i = 1; i < b2.size(); ++i) append(b2[i]);
  append(EncodeJobFinalTerminator(2));
  t.QueueRead(stream);

  std::vector<CapturedBand> bands;
  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, params, &pages, Collect(&bands));
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 2u);

  const auto per_page = ConcatBandsPerPage(bands);
  ASSERT_EQ(per_page.size(), 2u);
  // page_index is the 0-based device page index (pidx - 1).
  EXPECT_EQ(per_page.at(0), DecodeWholeJpegRgb(jpeg1));
  EXPECT_EQ(per_page.at(1), DecodeWholeJpegRgb(jpeg2));
}

// Core invariant, raw gray (GRAY64/C=NONE): band concatenation equals the
// page's decoded samples (which for raw gray are the payload bytes verbatim).
// Height is large enough to span several bands.
TEST(RunScan, StreamingGrayFlatbedBandsMatchWholePage) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,40,427,100,"));

  const int width = 40;
  const int height = 100;
  std::vector<uint8_t> raw(static_cast<size_t>(width) * height);
  for (size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<uint8_t>(i & 0xff);
  auto block = EncodeBlockHeader(static_cast<uint16_t>(width));
  block.insert(block.end(), raw.begin(), raw.end());
  t.QueueRead(block);
  t.QueueRead(EncodeJobFinalTerminator(1));

  auto params = GrayParams();
  params.area = brscan::Area{0, 0, width, height};

  std::vector<CapturedBand> bands;
  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, params, &pages, Collect(&bands));
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].data, raw);

  ASSERT_GT(bands.size(), 1u) << "a 100-row page must span several bands";
  for (const auto& b : bands) {
    EXPECT_EQ(b.format, brscan::PixelFormat::kGray);
    EXPECT_EQ(b.full_width, width);
    EXPECT_EQ(b.full_height, height);
  }
  const auto per_page = ConcatBandsPerPage(bands);
  ASSERT_EQ(per_page.size(), 1u);
  EXPECT_EQ(per_page.at(0), raw);
}

// Core invariant, bitonal (TEXT/C=RLENGTH): band concatenation equals the
// page's decoded 1-bpp samples.
TEST(RunScan, StreamingBlackWhiteBandsMatchWholePage) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  // width_px=9 (row_bytes = 2), height_px=40 -> spans several bands.
  const int height = 40;
  t.QueueRead(EncodeOfferFrame("300,300,2,292,9,427,40,"));

  std::vector<uint8_t> want;
  for (int row = 0; row < height; ++row) {
    const uint8_t a = static_cast<uint8_t>(row);
    const uint8_t b = static_cast<uint8_t>(row * 2 + 1);
    auto block = EncodeRlengthBlockHeader(0x42, 3);
    const std::vector<uint8_t> payload = {0x01, a, b};  // literal run of 2.
    block.insert(block.end(), payload.begin(), payload.end());
    t.QueueRead(block);
    want.push_back(a);
    want.push_back(b);
  }
  t.QueueRead(EncodeJobFinalTerminator(1));

  std::vector<CapturedBand> bands;
  std::vector<brscan::ScanResult> pages;
  const auto status =
      brscan::RunScan(t, BlackWhiteParams(), &pages, Collect(&bands));
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].data, want);

  ASSERT_GT(bands.size(), 1u);
  for (const auto& b : bands) {
    EXPECT_EQ(b.format, brscan::PixelFormat::kBitonal);
    EXPECT_EQ(b.full_width, 9);
    EXPECT_EQ(b.full_height, height);
  }
  const auto per_page = ConcatBandsPerPage(bands);
  ASSERT_EQ(per_page.size(), 1u);
  EXPECT_EQ(per_page.at(0), want);
}

// An empty BandCallback makes the streaming overload behave exactly like the
// three-argument overload (which is itself implemented by delegating here).
TEST(RunScan, StreamingEmptyCallbackMatchesNonStreaming) {
  const auto build = [](brscan::FakeTransport* t) {
    QueuePreamble(t);
    t->QueueRead(EncodeOfferFrame("300,300,2,292,3460,427,5052,"));
    const auto jpeg = MakeSyntheticJpeg(16, 8);
    auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
    payload.insert(payload.end(), jpeg.begin(), jpeg.end());
    t->QueueRead(payload);
    t->QueueRead(EncodeJobFinalTerminator(1));
  };

  brscan::FakeTransport t3;
  build(&t3);
  std::vector<brscan::ScanResult> pages3;
  ASSERT_EQ(brscan::RunScan(t3, ColorParams(), &pages3), brscan::Status::kOk);

  brscan::FakeTransport t4;
  build(&t4);
  std::vector<brscan::ScanResult> pages4;
  ASSERT_EQ(
      brscan::RunScan(t4, ColorParams(), &pages4, brscan::BandCallback{}),
      brscan::Status::kOk);

  ASSERT_EQ(pages3.size(), pages4.size());
  ASSERT_EQ(pages3.size(), 1u);
  EXPECT_EQ(pages3[0].data, pages4[0].data);
  EXPECT_EQ(pages3[0].width, pages4[0].width);
  EXPECT_EQ(pages3[0].height, pages4[0].height);
}

// Returning false from the callback cancels promptly: RunScan returns
// kCancelled and, as the one documented exception, keeps in `out` the pages
// that completed before the cancel (here page 1), dropping the rest.
TEST(RunScan, StreamingCancellationStopsPromptlyAndKeepsCompletedPages) {
  brscan::FakeTransport t;
  QueueConnectPreamble(&t);
  t.QueueRead(std::vector<uint8_t>{0x80, 0x00});  // ESC D ADF ack
  t.QueueTimeout();                               // drain done
  t.QueueRead(EncodeOfferFrame("300,300,1,292,3460,0,0,"));

  auto params = ColorParams();
  params.source = brscan::Source::kAdf;
  params.area = brscan::Area{0, 0, 16, 8};

  const auto jpeg1 = MakeSyntheticJpeg(16, 8);
  const auto jpeg2 = MakeSyntheticJpeg(16, 8);
  auto block1 = EncodeBlockHeader(static_cast<uint16_t>(jpeg1.size()), 1);
  block1.insert(block1.end(), jpeg1.begin(), jpeg1.end());
  t.QueueRead(block1);
  t.QueueRead(EncodeEndOfPageMarker(1));  // page 1 completes.
  auto block2 = EncodeBlockHeader(static_cast<uint16_t>(jpeg2.size()), 2);
  block2.insert(block2.end(), jpeg2.begin(), jpeg2.end());
  t.QueueRead(block2);
  t.QueueRead(EncodeJobFinalTerminator(2));

  // Cancel on the first band of page 2 (page_index 1).
  int seen = 0;
  const brscan::BandCallback cb = [&](const brscan::ScanBand& b) {
    ++seen;
    return b.page_index == 0;  // keep page 0, cancel when page 1 starts.
  };

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, params, &pages, cb);
  EXPECT_EQ(status, brscan::Status::kCancelled);
  EXPECT_GT(seen, 0);
  // Page 1 completed before the cancel and is kept; page 2 is dropped.
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].data, jpeg1);
}

// A malformed end-of-page marker (byte[0] isn't the 0x82 anchor) must
// surface as a protocol error, not a hang or a desync that misreads
// whatever bytes follow as a bogus next page.
TEST(RunScan, MalformedEndOfPageMarkerIsProtocolError) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,3460,427,5052,"));

  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);

  // Same shape as EncodeEndOfPageMarker(1) except byte[0] is 0x00, not the
  // required 0x82 anchor.
  t.QueueRead(std::vector<uint8_t>{0x00, 0x07, 0x00, 0x01, 0x00, 0x84, 0x00,
                                    0x00, 0x00, 0x00});

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(t, ColorParams(), &pages);
  EXPECT_EQ(status, brscan::Status::kProtocolError);
  EXPECT_TRUE(pages.empty());
}

// --- Scan-button flow (RunButtonScan) -------------------------------------

namespace {

// The config-command frame the printer pushes after ESC K: `0x30 <len>
// 0x00` then the KEY=VALUE payload (see docs/BUTTON.md's "Config command"
// and reference/protocol-notes-button-options.md). <len> is the payload's
// byte count. Carries no device identity or scan content.
std::vector<uint8_t> EncodeButtonConfigFrame(const std::string& payload) {
  std::vector<uint8_t> out;
  out.push_back(0x30);
  out.push_back(static_cast<uint8_t>(payload.size()));
  out.push_back(0x00);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// A baseline File/Color/PDF/Letter config payload, verbatim from
// reference/protocol-notes-button-options.md's captured fixtures.
const char* const kColorLetterConfigPayload =
    "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
    "T=PDF(Image)\nW=0\nG=0\nX=0\n";

// The Letter-size scan area at 300 dpi the daemon's callback would supply
// (reference/protocol-notes-button-options.md paper table).
const brscan::Area kButtonLetterArea = {478, 0, 2990, 3253};

// Queues the button-flow greeting only (no ESC Q reply, no source-select
// ack -- the button flow sends neither ESC Q nor ESC S/ESC D).
void QueueButtonGreeting(brscan::FakeTransport* t) {
  t->QueueRead(std::string("+OK 200\r\n"));
}

}  // namespace

// Full color scan-button session over a mock transport: greeting, ESC K ->
// config frame, ESC I -> offer, ESC X -> one JPEG page + job-final
// terminator. Asserts the callback saw the exact config-frame bytes, the
// returned page, and that the wire carried ESC K / button ESC I / button
// ESC X and NONE of ESC Q, ESC S, or ESC D.
TEST(RunButtonScan, ColorSessionReturnsPageAndSendsButtonCommands) {
  brscan::FakeTransport t;
  QueueButtonGreeting(&t);
  const auto config_frame = EncodeButtonConfigFrame(kColorLetterConfigPayload);
  t.QueueRead(config_frame);
  t.QueueRead(EncodeOfferFrame("300,300,1,292,3460,0,0,"));

  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto block = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()), 1);
  block.insert(block.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(block);
  t.QueueRead(EncodeJobFinalTerminator(1));

  std::vector<uint8_t> seen_config;
  int calls = 0;
  const brscan::ButtonParamsFn cb =
      [&](const std::vector<uint8_t>& config) -> std::optional<brscan::Params> {
    ++calls;
    seen_config = config;
    brscan::Params p;
    p.mode = brscan::ScanMode::kColor;
    p.x_dpi = 300;
    p.y_dpi = 300;
    p.area = kButtonLetterArea;
    return p;
  };

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunButtonScan(t, cb, &pages);
  ASSERT_EQ(status, brscan::Status::kOk);

  // (a) callback invoked once with the exact full config frame.
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(seen_config, config_frame);

  // (c) one page, correct dimensions/bytes.
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].format, brscan::PixelFormat::kRgb);
  EXPECT_EQ(pages[0].width, 16);
  EXPECT_EQ(pages[0].height, 8);
  EXPECT_EQ(pages[0].data, jpeg);

  // (b) the wire carried ESC K, button ESC I (S=NORMAL_SCAN), button ESC X
  // (with the returned area), and NO ESC Q / ESC S / ESC D.
  brscan::Params exec = *cb(config_frame);  // same Params the flow computed.
  exec.button_flow = true;
  EXPECT_TRUE(Contains(t.written(), brscan::EncodeButtonQuery()))
      << "button flow must issue ESC K";
  EXPECT_TRUE(Contains(t.written(),
                       brscan::EncodeInfo(300, 300, brscan::ScanMode::kColor,
                                          /*duplex=*/false,
                                          /*button_flow=*/true)))
      << "button flow must issue ESC I with S=NORMAL_SCAN";
  EXPECT_TRUE(Contains(t.written(), brscan::EncodeExecute(exec)))
      << "button flow must issue the button ESC X with the returned area";
  // 0x1b 0x51 = ESC Q, 0x1b 0x53 = ESC S, 0x1b 0x44 = ESC D.
  EXPECT_FALSE(Contains(t.written(), {0x1b, 0x51})) << "no ESC Q in button flow";
  EXPECT_FALSE(Contains(t.written(), {0x1b, 0x53})) << "no ESC S in button flow";
  EXPECT_FALSE(Contains(t.written(), {0x1b, 0x44})) << "no ESC D in button flow";
}

// Multi-page color scan-button session: two JPEG pages, proving the shared
// RunReadout page loop is wired into the button flow too (not just RunScan).
TEST(RunButtonScan, ColorMultiPageReturnsAllPages) {
  brscan::FakeTransport t;
  QueueButtonGreeting(&t);
  t.QueueRead(EncodeButtonConfigFrame(kColorLetterConfigPayload));
  t.QueueRead(EncodeOfferFrame("300,300,1,292,3460,0,0,"));

  const auto jpeg1 = MakeSyntheticJpeg(16, 8);
  const auto jpeg2 = MakeSyntheticJpeg(16, 8);
  auto block1 = EncodeBlockHeader(static_cast<uint16_t>(jpeg1.size()), 1);
  block1.insert(block1.end(), jpeg1.begin(), jpeg1.end());
  t.QueueRead(block1);
  t.QueueRead(EncodeEndOfPageMarker(1));  // tail: next page's `64 07`.
  auto block2 = EncodeBlockHeader(static_cast<uint16_t>(jpeg2.size()), 2);
  block2.insert(block2.end(), jpeg2.begin(), jpeg2.end());
  t.QueueRead(block2);
  t.QueueRead(EncodeJobFinalTerminator(2));  // tail: `80 80`, job done.

  const brscan::ButtonParamsFn cb =
      [&](const std::vector<uint8_t>&) -> std::optional<brscan::Params> {
    brscan::Params p;
    p.mode = brscan::ScanMode::kColor;
    p.x_dpi = 300;
    p.y_dpi = 300;
    p.area = kButtonLetterArea;
    return p;
  };

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunButtonScan(t, cb, &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[0].data, jpeg1);
  EXPECT_EQ(pages[1].data, jpeg2);
}

// The real device terminates a scan-button job with a single 0x80 (then
// closes the connection), not the driver flow's 0x80 0x80. Confirmed byte
// for byte in reference/brscan-daemon-live.pcap and the vendor's own
// reference/brscan-button-options.pcap. RunButtonScan must end the job on
// this single-byte terminator instead of blocking for a second 0x80 that
// never arrives.
TEST(RunButtonScan, SingleByteJobFinalTerminatorEndsScan) {
  brscan::FakeTransport t;
  QueueButtonGreeting(&t);
  t.QueueRead(EncodeButtonConfigFrame(kColorLetterConfigPayload));
  t.QueueRead(EncodeOfferFrame("300,300,1,292,3460,0,0,"));

  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto block = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()), 1);
  block.insert(block.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(block);
  t.QueueRead(EncodeButtonJobFinal(1));

  const brscan::ButtonParamsFn cb =
      [&](const std::vector<uint8_t>&) -> std::optional<brscan::Params> {
    brscan::Params p;
    p.mode = brscan::ScanMode::kColor;
    p.x_dpi = 300;
    p.y_dpi = 300;
    p.area = kButtonLetterArea;
    return p;
  };

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunButtonScan(t, cb, &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].data, jpeg);
}

// The exact live scenario: two color pages, the first closed by the plain
// end-of-page marker (followed by the next page's block header) and the
// second -- the job's last page -- closed by the single-0x80 terminator.
// Mirrors ColorMultiPageReturnsAllPages but with EncodeButtonJobFinal in
// place of the driver flow's EncodeJobFinalTerminator.
TEST(RunButtonScan, MultiPageSingleByteJobFinalReturnsAllPages) {
  brscan::FakeTransport t;
  QueueButtonGreeting(&t);
  t.QueueRead(EncodeButtonConfigFrame(kColorLetterConfigPayload));
  t.QueueRead(EncodeOfferFrame("300,300,1,292,3460,0,0,"));

  const auto jpeg1 = MakeSyntheticJpeg(16, 8);
  const auto jpeg2 = MakeSyntheticJpeg(16, 8);
  auto block1 = EncodeBlockHeader(static_cast<uint16_t>(jpeg1.size()), 1);
  block1.insert(block1.end(), jpeg1.begin(), jpeg1.end());
  t.QueueRead(block1);
  t.QueueRead(EncodeEndOfPageMarker(1));  // tail: next page's `64 07`.
  auto block2 = EncodeBlockHeader(static_cast<uint16_t>(jpeg2.size()), 2);
  block2.insert(block2.end(), jpeg2.begin(), jpeg2.end());
  t.QueueRead(block2);
  t.QueueRead(EncodeButtonJobFinal(2));  // tail: single `80`, job done.

  const brscan::ButtonParamsFn cb =
      [&](const std::vector<uint8_t>&) -> std::optional<brscan::Params> {
    brscan::Params p;
    p.mode = brscan::ScanMode::kColor;
    p.x_dpi = 300;
    p.y_dpi = 300;
    p.area = kButtonLetterArea;
    return p;
  };

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunButtonScan(t, cb, &pages);
  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[0].data, jpeg1);
  EXPECT_EQ(pages[1].data, jpeg2);
}

// A callback returning std::nullopt (an unusable config) aborts the session
// as a protocol error, with no partial pages.
TEST(RunButtonScan, NulloptCallbackReportsProtocolError) {
  brscan::FakeTransport t;
  QueueButtonGreeting(&t);
  const auto config_frame = EncodeButtonConfigFrame(kColorLetterConfigPayload);
  t.QueueRead(config_frame);

  std::vector<uint8_t> seen_config;
  const brscan::ButtonParamsFn cb =
      [&](const std::vector<uint8_t>& config) -> std::optional<brscan::Params> {
    seen_config = config;
    return std::nullopt;
  };

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunButtonScan(t, cb, &pages);
  EXPECT_EQ(status, brscan::Status::kProtocolError);
  EXPECT_TRUE(pages.empty());
  // The callback is still handed the full config frame before it declines.
  EXPECT_EQ(seen_config, config_frame);
}

// A malformed config frame (bad leading byte) is rejected before the
// callback is ever consulted.
TEST(RunButtonScan, MalformedConfigFrameReportsProtocolError) {
  brscan::FakeTransport t;
  QueueButtonGreeting(&t);
  // Header byte 0 is 0x31, not the required 0x30 -- malformed.
  t.QueueRead(std::vector<uint8_t>{0x31, 0x02, 0x00, 0x41, 0x42});

  int calls = 0;
  const brscan::ButtonParamsFn cb =
      [&](const std::vector<uint8_t>&) -> std::optional<brscan::Params> {
    ++calls;
    return brscan::Params{};
  };

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunButtonScan(t, cb, &pages);
  EXPECT_EQ(status, brscan::Status::kProtocolError);
  EXPECT_TRUE(pages.empty());
  EXPECT_EQ(calls, 0) << "malformed frame must not reach the callback";
}

// A busy greeting (-NG 401) maps to kBusy, exactly as in RunScan, before any
// button command is sent or the callback is consulted.
TEST(RunButtonScan, BusyGreetingReportsBusy) {
  brscan::FakeTransport t;
  t.QueueRead(std::string("-NG 401\r\n"));

  int calls = 0;
  const brscan::ButtonParamsFn cb =
      [&](const std::vector<uint8_t>&) -> std::optional<brscan::Params> {
    ++calls;
    return brscan::Params{};
  };

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunButtonScan(t, cb, &pages);
  EXPECT_EQ(status, brscan::Status::kBusy);
  EXPECT_TRUE(pages.empty());
  EXPECT_EQ(calls, 0);
}

// Live, opt-in: connects to a real device and runs a small color flatbed
// scan end to end. Mirrors TcpTransportLive.GreetsWithOk in
// tests/session_test.cpp (Task 4). Skipped unless BRSCAN_TEST_HOST is set,
// so this never runs in ordinary hermetic test runs.
TEST(RunScanLive, ColorFlatbedScanAtLowResolution) {
  const char* host = std::getenv("BRSCAN_TEST_HOST");
  if (host == nullptr) GTEST_SKIP() << "set BRSCAN_TEST_HOST to run";

  brscan::TcpTransport transport(host, 54921);
  ASSERT_EQ(transport.Connect(), brscan::Status::kOk);

  brscan::Params params;
  params.mode = brscan::ScanMode::kColor;
  params.source = brscan::Source::kFlatbed;
  params.x_dpi = 100;
  params.y_dpi = 100;
  // A small crop keeps the live test fast rather than scanning the full
  // flatbed at even this low resolution.
  params.area = brscan::Area{0, 0, 200, 200};

  std::vector<brscan::ScanResult> pages;
  const auto status = brscan::RunScan(transport, params, &pages);
  transport.Disconnect();

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages[0].format, brscan::PixelFormat::kRgb);
  EXPECT_GT(pages[0].width, 0);
  EXPECT_GT(pages[0].height, 0);
  ASSERT_GE(pages[0].data.size(), 2u);
  EXPECT_EQ(pages[0].data[0], 0xff);
  EXPECT_EQ(pages[0].data[1], 0xd8);
}
