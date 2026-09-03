#include "brscan/scanner.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <turbojpeg.h>

#include "brscan/transport_tcp.h"
#include "brscan/types.h"
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
// width in the last two bytes. The other bytes are arbitrary filler, as
// their meaning is unconfirmed (see response.h).
std::vector<uint8_t> EncodeBlockHeader(uint16_t width) {
  return {0x00, 0x64, 0x07, 0x00, 0x01, 0x00, 0x84, 0xc0, 0x01, 0x00, 0x00,
          static_cast<uint8_t>(width & 0xff),
          static_cast<uint8_t>((width >> 8) & 0xff)};
}

// The 12-byte block header shape the real device actually sends (anchors
// at offsets 1 and 5, i.e. EncodeBlockHeader's 13-byte shape with its
// leading 0x00 dropped; see DetectHeaderLength's doc comment in
// scanner.cpp). The 13-byte shape above is the one
// reference/streams/s0_in.bin (an older vendor-driver capture) shows;
// this one is what a live probe against the real device for this task
// found instead.
std::vector<uint8_t> EncodeBlockHeader12(uint16_t width) {
  return {0x64, 0x07, 0x00, 0x01, 0x00, 0x84, 0xc0, 0x01, 0x00, 0x00,
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
std::vector<uint8_t> EncodeRlengthBlockHeader(uint8_t type, uint16_t length) {
  return {type, 0x07, 0x00, 0x01, 0x00, 0x84, 0x00, 0x00, 0x00, 0x00,
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

// Queues the connection preamble common to every successful scan: a ready
// greeting, an ESC Q reply (arbitrary content, drained and discarded), and
// the flatbed select ack.
void QueuePreamble(brscan::FakeTransport* t) {
  t->QueueRead(std::string("+OK 200\r\n"));
  t->QueueRead(std::vector<uint8_t>{0xc1, 0x00, 0x35, 0x0a});  // ESC Q reply
  t->QueueTimeout();                                           // drain done
  t->QueueRead(std::vector<uint8_t>{0x80, 0x00});              // ESC S ack
  t->QueueTimeout();                                           // drain done
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

  brscan::ScanResult result;
  const auto status = brscan::RunScan(t, ColorParams(), &result);
  ASSERT_EQ(status, brscan::Status::kOk);
  EXPECT_EQ(result.format, brscan::PixelFormat::kRgb);
  EXPECT_EQ(result.width, 16);
  EXPECT_EQ(result.height, 8);
  ASSERT_EQ(result.data, jpeg);
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

  brscan::ScanResult result;
  const auto status = brscan::RunScan(t, ColorParams(), &result);
  ASSERT_EQ(status, brscan::Status::kOk);
  EXPECT_EQ(result.format, brscan::PixelFormat::kRgb);
  EXPECT_EQ(result.width, 300);
  EXPECT_EQ(result.height, 300);
  ASSERT_EQ(result.data, jpeg);
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

  brscan::ScanResult result;
  const auto status = brscan::RunScan(t, ColorParams(), &result);
  ASSERT_EQ(status, brscan::Status::kOk);
  EXPECT_EQ(result.format, brscan::PixelFormat::kRgb);
  EXPECT_EQ(result.width, 300);
  EXPECT_EQ(result.height, 300);
  ASSERT_EQ(result.data, jpeg);
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

  brscan::ScanResult result;
  const auto status = brscan::RunScan(t, GrayParams(), &result);
  ASSERT_EQ(status, brscan::Status::kOk);
  EXPECT_EQ(result.format, brscan::PixelFormat::kGray);
  EXPECT_EQ(result.width, 4);
  EXPECT_EQ(result.height, 3);
  EXPECT_EQ(result.data, raw);
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

  brscan::ScanResult result;
  const auto status = brscan::RunScan(t, BlackWhiteParams(), &result);
  ASSERT_EQ(status, brscan::Status::kOk);
  EXPECT_EQ(result.format, brscan::PixelFormat::kBitonal);
  EXPECT_EQ(result.width, 9);
  EXPECT_EQ(result.height, 2);
  const std::vector<uint8_t> want = {0xAA, 0xBB, 0xCC, 0xDD};
  EXPECT_EQ(result.data, want);
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

  brscan::ScanResult result;
  const auto status = brscan::RunScan(t, TrueGrayParams(), &result);
  ASSERT_EQ(status, brscan::Status::kOk);
  EXPECT_EQ(result.format, brscan::PixelFormat::kGray);
  EXPECT_EQ(result.width, 4);
  EXPECT_EQ(result.height, 3);
  std::vector<uint8_t> want;
  for (const auto& row : rows) want.insert(want.end(), row.begin(), row.end());
  EXPECT_EQ(result.data, want);
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

  brscan::ScanResult result;
  const auto status = brscan::RunScan(t, BlackWhiteParams(), &result);
  EXPECT_NE(status, brscan::Status::kOk);
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

  brscan::ScanResult result;
  const auto status = brscan::RunScan(t, BlackWhiteParams(), &result);
  EXPECT_EQ(status, brscan::Status::kProtocolError);
}

TEST(RunScan, BusyGreetingReportsBusy) {
  brscan::FakeTransport t;
  t.QueueRead(std::string("-NG 401\r\n"));

  brscan::ScanResult result;
  const auto status = brscan::RunScan(t, ColorParams(), &result);
  EXPECT_EQ(status, brscan::Status::kBusy);
}

TEST(RunScan, AdfSourceSelectNoAckReportsNoPaper) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  // ESC D ADF gets no reply at all (queue goes quiet immediately): the
  // heuristic mapping for an empty feeder. See scanner.cpp's caveat on
  // this mapping.
  t.QueueTimeout();

  auto params = ColorParams();
  params.source = brscan::Source::kAdf;

  brscan::ScanResult result;
  const auto status = brscan::RunScan(t, params, &result);
  EXPECT_EQ(status, brscan::Status::kNoPaper);
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

  brscan::ScanResult result;
  const auto status = brscan::RunScan(t, ColorParams(), &result);
  EXPECT_NE(status, brscan::Status::kOk);
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

  brscan::ScanResult result;
  const auto status = brscan::RunScan(t, GrayParams(), &result);
  EXPECT_NE(status, brscan::Status::kOk);
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

  brscan::ScanResult result;
  const auto status = brscan::RunScan(transport, params, &result);
  transport.Disconnect();

  ASSERT_EQ(status, brscan::Status::kOk);
  EXPECT_EQ(result.format, brscan::PixelFormat::kRgb);
  EXPECT_GT(result.width, 0);
  EXPECT_GT(result.height, 0);
  ASSERT_GE(result.data.size(), 2u);
  EXPECT_EQ(result.data[0], 0xff);
  EXPECT_EQ(result.data[1], 0xd8);
}
