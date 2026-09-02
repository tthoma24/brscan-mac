#include "scanner.h"

#include <string>

#include "command.h"
#include "decode_jpeg.h"
#include "response.h"
#include "session.h"

namespace brscan {

namespace {

// Timeout for a reply we expect to be small and prompt: the ESC Q drain,
// the ESC S / ESC D source-select ack, and the ESC I offer. Generous
// relative to the sub-second replies observed live, to tolerate a slow
// network without false-timing-out a healthy device.
constexpr int kAckTimeoutMs = 5000;

// Timeout for a single Read() during the execute phase: waiting for the
// first block header (the device may take a few seconds to warm up the
// lamp and start the mechanical scan) and for each chunk of the image
// payload after that. Any gap longer than this is treated as a
// stalled/cancelled scan per docs/PROTOCOL.md ("Cancellation": a
// device-panel cancel emits no status, the stream just stops).
constexpr int kScanTimeoutMs = 20000;

// How long DrainQuiet waits, after the first chunk of a reply, for a
// *further* chunk before deciding the reply is finished. 800 ms is chosen
// relative to kAckTimeoutMs (5000 ms, the bound on the whole ack/reply):
// every drained reply observed live (the ESC Q capability block, the ESC
// S/D source-select ack) arrived as one or two chunks within tens of
// milliseconds of each other on the same LAN connection, so 800 ms is
// generous headroom over that without eating meaningfully into the 5 s
// budget. It is NOT a formal bound on inter-chunk gaps -- see DrainQuiet's
// doc comment for the residual risk this leaves.
constexpr int kDrainIdleTimeoutMs = 800;

// The device caps every payload block at this many bytes; a block header's
// trailing length field pins at this exact value (its documented sentinel,
// 0xfff4) precisely when the block is full and more blocks follow. See the
// long comment above ReadChunkedJpeg for how this was found.
constexpr int kMaxChunkBytes = 0xfff4;

// Buffers reads from a Transport and consumes them structurally: exact
// byte counts or "peek ahead without consuming". A real TCP stream can
// deliver either split across arbitrarily many underlying Read() calls, so
// every method here loops calling Fill() until it has what it needs (or a
// Read() reports a non-kOk status, which is propagated immediately).
class Framer {
 public:
  explicit Framer(Transport* transport) : transport_(transport) {}

  // Reads one Transport::Read chunk into the internal buffer.
  Status Fill(int timeout_ms) {
    uint8_t chunk[65536];
    size_t got = 0;
    const Status s = transport_->Read(chunk, sizeof(chunk), &got, timeout_ms);
    if (s != Status::kOk) return s;
    buf_.insert(buf_.end(), chunk, chunk + got);
    return Status::kOk;
  }

  // Ensures at least `n` bytes are buffered, then removes and returns the
  // first `n` of them.
  Status ReadExact(size_t n, int timeout_ms, std::vector<uint8_t>* out) {
    while (buf_.size() < n) {
      const Status s = Fill(timeout_ms);
      if (s != Status::kOk) return s;
    }
    out->assign(buf_.begin(), buf_.begin() + n);
    buf_.erase(buf_.begin(), buf_.begin() + n);
    return Status::kOk;
  }

  // Waits for the first chunk of a reply, then keeps consuming further
  // chunks until a Fill() call times out (the reply has gone quiet),
  // discarding everything read. Used for replies with no confirmed fixed
  // length and no self-describing length prefix: the ESC Q capability
  // block, and the ESC S / ESC D source-select ack (see docs/PROTOCOL.md
  // and RunScan's comments on each). Every other reply in the flow either
  // has a known fixed size, carries its own length, or is a payload block
  // read via ReadExact with a length taken from its header instead.
  //
  // Residual risk: this is a heuristic, not a protocol-confirmed framing.
  // If a genuine reply legitimately pauses mid-transmission for longer
  // than `idle_timeout_ms` (see kDrainIdleTimeoutMs's justification for
  // why that's not expected in practice), DrainQuiet returns early and
  // its late-arriving tail bytes land in front of whatever the *next*
  // read expects, corrupting it. There's no self-describing length here
  // to detect that case cleanly; see docs/PROTOCOL.md for why these two
  // replies (ESC Q's capability block, ESC S/D's source-select ack) have
  // no such length in the first place.
  Status DrainQuiet(int first_timeout_ms, int idle_timeout_ms) {
    Status s = Fill(first_timeout_ms);
    if (s != Status::kOk) return s;
    for (;;) {
      s = Fill(idle_timeout_ms);
      if (s == Status::kTimeout) break;
      if (s != Status::kOk) return s;
    }
    buf_.clear();
    return Status::kOk;
  }

  // Ensures at least `n` bytes are buffered and returns a copy of the
  // first `n` of them, without consuming them (unlike ReadExact). Used to
  // inspect a reply's shape before deciding how many bytes actually belong
  // to it.
  Status Peek(size_t n, int timeout_ms, std::vector<uint8_t>* out) {
    while (buf_.size() < n) {
      const Status s = Fill(timeout_ms);
      if (s != Status::kOk) return s;
    }
    out->assign(buf_.begin(), buf_.begin() + n);
    return Status::kOk;
  }

 private:
  Transport* transport_;
  std::vector<uint8_t> buf_;
};

// The block header ParseBlockHeader validates is a fixed 13-byte shape
// with anchor bytes at offsets 2 and 6 (0x07 and 0x84) -- confirmed from
// reference/streams/s0_in.bin, a capture made with the vendor driver. A
// live probe against the real device for this task found a 12-byte
// variant instead, with the same anchors shifted one byte earlier (1 and
// 5): apparently current firmware omits a constant leading 0x00 byte the
// old capture had. Detect which shape is on the wire by peeking rather
// than assuming either one, so this keeps working across that drift (and
// keeps ParseBlockHeader itself, and its existing tests, untouched).
Status DetectHeaderLength(Framer* framer, int timeout_ms, int* header_len) {
  std::vector<uint8_t> peek;
  const Status s = framer->Peek(7, timeout_ms, &peek);
  if (s != Status::kOk) return s;
  if (peek[2] == 0x07 && peek[6] == 0x84) {
    *header_len = 13;
    return Status::kOk;
  }
  if (peek[1] == 0x07 && peek[5] == 0x84) {
    *header_len = 12;
    return Status::kOk;
  }
  return Status::kProtocolError;
}

// Reads one block header (12 or 13 bytes, per DetectHeaderLength) and
// parses it.
Status ReadBlockHeader(Framer* framer, int timeout_ms, BlockHeader* header) {
  int header_len = 0;
  Status s = DetectHeaderLength(framer, timeout_ms, &header_len);
  if (s != Status::kOk) return s;
  std::vector<uint8_t> header_bytes;
  s = framer->ReadExact(static_cast<size_t>(header_len), timeout_ms, &header_bytes);
  if (s != Status::kOk) return s;
  // ParseBlockHeader expects the legacy 13-byte shape; normalize a
  // detected 12-byte header onto it by restoring the dropped leading byte.
  // Its value doesn't matter -- only the anchor bytes and the trailing
  // length/width field, both preserved by this shift, are read.
  if (header_len == 12) header_bytes.insert(header_bytes.begin(), uint8_t{0});
  const auto parsed = ParseBlockHeader(header_bytes.data(), header_bytes.size());
  if (!parsed.has_value()) return Status::kProtocolError;
  *header = *parsed;
  return Status::kOk;
}

// Reads a full JPEG payload, which may span multiple network blocks.
//
// This is the key discovery of this task, found by scanning a real
// 1153x1684 color flatbed scan for its JPEG EOI and getting a file that
// decoded its header fine but failed full decompression with "Unsupported
// marker type" at libjpeg-turbo's actual entropy-decode step -- while the
// same bytes read back as a well-formed 345555-byte JPEG to `sips`/`file`.
// The bad marker sat at byte offset 65535, and dumping the raw stream
// around every 0x10000-ish boundary showed a repeating pattern: a second
// 12-byte block header (same anchors as the first) spliced into the
// stream every exactly 65524 bytes. BlockHeader::width's confirmed
// "JPEG-length sentinel" behavior from Task 6 (response.h) turns out to be
// this same mechanism seen from a single-block sample: the field is each
// block's own payload length, capped at kMaxChunkBytes (0xfff4 = 65524)
// while more blocks remain, and the true remaining length on the final
// block. A payload under one block's worth is just the degenerate case of
// this loop running once.
//
// Residual risk: a final block whose payload is exactly kMaxChunkBytes
// (0xfff4) is indistinguishable on the wire from a full, still-continuing
// block -- both declare the same sentinel length. That case is not known
// to have been observed (every payload seen live ended with a shorter
// final block), and there's no way to tell them apart from the header
// alone; if it ever occurs, this loop will call ReadBlockHeader again and
// block until kScanTimeoutMs elapses waiting for a header that isn't
// coming, surfacing as Status::kTimeout rather than a hang.
Status ReadChunkedJpeg(Framer* framer, const BlockHeader& first_header, int timeout_ms,
                        std::vector<uint8_t>* jpeg) {
  jpeg->clear();
  BlockHeader header = first_header;
  for (;;) {
    if (header.width <= 0) return Status::kProtocolError;
    const size_t to_read = header.width == kMaxChunkBytes
                                ? static_cast<size_t>(kMaxChunkBytes)
                                : static_cast<size_t>(header.width);
    std::vector<uint8_t> chunk;
    const Status s = framer->ReadExact(to_read, timeout_ms, &chunk);
    if (s != Status::kOk) return s;
    jpeg->insert(jpeg->end(), chunk.begin(), chunk.end());
    if (header.width != kMaxChunkBytes) break;  // that was the final block.
    const Status header_status = ReadBlockHeader(framer, timeout_ms, &header);
    if (header_status != Status::kOk) return header_status;
  }
  // Defensive cross-check on top of the length-driven read above: a
  // well-formed payload always ends at the JPEG EOI marker.
  if (jpeg->size() < 2 || (*jpeg)[jpeg->size() - 2] != 0xff ||
      (*jpeg)[jpeg->size() - 1] != 0xd9) {
    return Status::kProtocolError;
  }
  return Status::kOk;
}

// Reads a full raw gray payload: exactly `total_bytes`, with no embedded
// headers to skip.
//
// This is NOT symmetric with ReadChunkedJpeg, which was the working
// assumption until a live probe against the real device disproved it: a
// gray scan large enough to cross the same kMaxChunkBytes boundary a
// color scan chunks at (a 300x300-pixel-area gray scan, 90000 bytes) read
// straight through with no header spliced in -- the bytes at that offset
// were plain 0xff pixel data (blank glass), not an anchor match. The
// mid-stream headers seen for color are apparently tied to how the
// device's JPEG encoder flushes its output in bounded bursts, not a
// general network-layer framing; raw (GRAY64/C=NONE) mode has no encoder
// in the loop and is just one continuous byte stream.
Status ReadRawGray(Framer* framer, size_t total_bytes, int timeout_ms,
                    std::vector<uint8_t>* raw) {
  return framer->ReadExact(total_bytes, timeout_ms, raw);
}

}  // namespace

Status RunScan(Transport& transport, const Params& params, ScanResult* out) {
  if (out == nullptr) return Status::kProtocolError;

  // Greeting. Session::Open() already maps +OK 200 -> kOk, -NG 401 ->
  // kBusy, anything else -> kProtocolError; reuse it rather than
  // reimplementing that parse.
  Status status = Session(&transport).Open();
  if (status != Status::kOk) return status;

  Framer framer(&transport);
  const auto send = [&](const std::vector<uint8_t>& bytes) {
    return transport.Write(bytes.data(), bytes.size());
  };

  // ESC Q: session init, sent once per connection. Its reply carries no
  // information this flow needs (see docs/PROTOCOL.md: "Not required for
  // basic scanning"), so it is drained and discarded rather than decoded.
  status = send(EncodeQuery());
  if (status != Status::kOk) return status;
  status = framer.DrainQuiet(kAckTimeoutMs, kDrainIdleTimeoutMs);
  if (status != Status::kOk) return status;

  // Select source. ESC S / ESC D each ack with a short reply whose exact
  // length isn't stable: the capture in reference/streams/s0_in.bin shows
  // 2 bytes for ESC S, but a live probe against the real device for this
  // task saw as little as 1 byte for the same command. Drain it like the
  // ESC Q reply rather than assuming a fixed count.
  status = send(EncodeSelectFlatbed());
  if (status != Status::kOk) return status;
  status = framer.DrainQuiet(kAckTimeoutMs, kDrainIdleTimeoutMs);
  if (status != Status::kOk) return status;

  if (params.source == Source::kAdf) {
    status = send(EncodeSelectAdf());
    if (status != Status::kOk) return status;
    status = framer.DrainQuiet(kAckTimeoutMs, kDrainIdleTimeoutMs);
    if (status == Status::kTimeout) {
      // No response fixture for the ADF-empty case was ever captured (see
      // tests/fixtures/README.md: it's listed as a gap, not something
      // Task 6 extracted); the vendor driver's own command stream in
      // reference/streams/s0_out.bin simply stops right after this
      // command in the no-paper test. Heuristic, not confirmed: map a
      // source-select ack that doesn't arrive at all -- when every other
      // observed ack arrived in well under a second -- to kNoPaper rather
      // than a generic kTimeout.
      return Status::kNoPaper;
    }
    if (status != Status::kOk) return status;
  }

  // Per-scan negotiate: request the caller's resolution/mode/duplex and
  // read the granted offer.
  status = send(EncodeInfo(params.x_dpi, params.y_dpi, params.mode, params.duplex));
  if (status != Status::kOk) return status;

  // The offer reply is [1-byte status][2-byte LE length][text][NUL]. A
  // live probe against the real device (and a closer re-read of
  // reference/streams/s0_in.bin) placed this status byte precisely: it's
  // a separate byte here, not the last byte of the preceding ESC Q reply
  // as this task's initial protocol-notes.md-only analysis assumed. Its
  // value (0x00 in every sample seen) isn't validated, only skipped,
  // since nothing in this flow depends on it.
  std::vector<uint8_t> offer_status;
  status = framer.ReadExact(1, kAckTimeoutMs, &offer_status);
  if (status != Status::kOk) return status;

  std::vector<uint8_t> len_bytes;
  status = framer.ReadExact(2, kAckTimeoutMs, &len_bytes);
  if (status != Status::kOk) return status;
  const size_t offer_len =
      static_cast<size_t>(len_bytes[0]) | (static_cast<size_t>(len_bytes[1]) << 8);

  std::vector<uint8_t> offer_bytes;
  status = framer.ReadExact(offer_len, kAckTimeoutMs, &offer_bytes);
  if (status != Status::kOk) return status;
  // The offer text is followed by a trailing NUL inside its length-prefixed
  // frame (confirmed in every captured sample); strip it before handing
  // the CSV to ParseOffer, which expects the text to end at its own
  // terminating comma.
  if (!offer_bytes.empty() && offer_bytes.back() == 0) offer_bytes.pop_back();
  const std::string offer_csv(offer_bytes.begin(), offer_bytes.end());
  const auto offer = ParseOffer(offer_csv);
  if (!offer.has_value()) return Status::kProtocolError;

  // An all-zero Params::area means "not set by the caller": request the
  // full area the offer just granted.
  Params exec_params = params;
  const Area& a = exec_params.area;
  if (a.x0 == 0 && a.y0 == 0 && a.x1 == 0 && a.y1 == 0) {
    exec_params.area = Area{0, 0, offer->width_px, offer->height_px};
  }

  // Execute.
  status = send(EncodeExecute(exec_params));
  if (status != Status::kOk) return status;

  BlockHeader header;
  status = ReadBlockHeader(&framer, kScanTimeoutMs, &header);
  if (status != Status::kOk) return status;

  if (params.mode == ScanMode::kColor) {
    std::vector<uint8_t> jpeg;
    status = ReadChunkedJpeg(&framer, header, kScanTimeoutMs, &jpeg);
    if (status != Status::kOk) return status;

    Image image;
    const Status decode_status = DecodeJpeg(jpeg.data(), jpeg.size(), &image);
    if (decode_status != Status::kOk) return decode_status;

    out->format = PixelFormat::kRgb;
    out->width = image.width;
    out->height = image.height;
    out->data = std::move(jpeg);
    return Status::kOk;
  }

  // Gray: raw payload, exactly width * height bytes, no embedded headers
  // (see ReadRawGray). Width comes from the first block's header
  // (confirmed reliable for a gray payload); height comes from the
  // requested area, since gray has no length field analogous to the JPEG
  // case to confirm it against.
  //
  // Residual risk: `height` here is what we *asked for*, not something the
  // device confirms back -- unlike color, where the actual JPEG dimensions
  // come from the decoded image itself. If the device ever auto-crops a
  // gray scan's delivered height short of the requested area (as Task 6's
  // report notes it does for some same-offer color scans, trimming to
  // detected paper edges), ReadRawGray would block waiting for bytes that
  // never arrive rather than returning the shorter image, surfacing as
  // Status::kTimeout. Not observed in this task's live gray scans, but
  // not independently ruled out either.
  const int width = header.width;
  const int height = exec_params.area.y1 - exec_params.area.y0;
  if (width <= 0 || height <= 0) return Status::kProtocolError;
  const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height);

  std::vector<uint8_t> raw;
  status = ReadRawGray(&framer, expected, kScanTimeoutMs, &raw);
  if (status != Status::kOk) return status;

  Image image;
  const Status decode_status = DecodeGrayRaw(width, height, raw.data(), raw.size(), &image);
  if (decode_status != Status::kOk) return decode_status;

  out->format = PixelFormat::kGray;
  out->width = width;
  out->height = height;
  out->data = std::move(raw);
  return Status::kOk;
}

}  // namespace brscan
