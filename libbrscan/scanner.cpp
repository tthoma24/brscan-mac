#include "brscan/scanner.h"

#include <algorithm>
#include <map>
#include <string>

#include "brscan/session.h"
#include "command.h"
#include "decode_jpeg.h"
#include "decode_rlength.h"
#include "response.h"

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

// The device caps every payload block at this many bytes. A block header's
// trailing length field pins at this exact value (0xfff4) as a "more data
// follows" SENTINEL, NOT as an exact byte count: a chunk may declare
// 0xfff4 yet carry FEWER bytes before the next block header or the
// end-of-page marker (observed on real ADF hardware -- see the long
// comment above ReadChunkedJpeg). A length strictly below 0xfff4 is an
// honest final-chunk length.
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

  // Like Peek, but tolerant of the stream ending before `n` bytes arrive:
  // fills until the buffer holds at least `n` bytes OR a Fill() times out
  // (no more data is coming right now), then returns a copy of whatever is
  // buffered (at most `n` bytes), without consuming it. A non-timeout error
  // is propagated. Used to look ahead for a chunk boundary near the end of
  // the payload, where the boundary itself (and the short tail after it)
  // can arrive well before `n` further bytes would -- so a plain Peek(n)
  // there would time out with nothing instead of surfacing the boundary.
  Status PeekUpTo(size_t n, int timeout_ms, std::vector<uint8_t>* out) {
    while (buf_.size() < n) {
      const Status s = Fill(timeout_ms);
      if (s == Status::kTimeout) break;
      if (s != Status::kOk) return s;
    }
    const size_t take = std::min(n, buf_.size());
    out->assign(buf_.begin(), buf_.begin() + take);
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

// Scans a look-ahead `window` (which must begin at the start of a chunk's
// data, never inside a header) for the first chunk boundary at an offset
// in [1, kMaxChunkBytes]. A boundary is the first position after the chunk
// data where either the next block header or the end-of-page marker begins:
//   - block header (12-byte shape): <type> 07 00 <pidx> 00 84 .. .. 00 00,
//     type in {0x64, 0x40, 0x42};
//   - end-of-page marker: 82 07 00 <pidx> 00 84 00 00 00 00.
// Both shapes are matched on all of their fixed bytes -- the 07/84 anchors
// AND the constant zero bytes -- not just a two- or three-byte anchor.
// That precision matters: a looser 0x82/0x07/../0x84 marker test (three
// bytes) false-matches ordinary JPEG entropy about once every 16 MB, and
// real ADF duplex page 1 (~3.7 MB) hit exactly such a false positive
// mid-payload, truncating the page. The full fixed-byte match survived
// both the ~10 MB simplex and duplex captures with no false positive.
//
// On a match, sets *pos to the boundary offset (the sentinel chunk's true
// data length) and *is_end_of_page to which pattern matched, and returns
// true. Returns false when no boundary appears within the scanned range --
// a genuinely full chunk that still continues. The scan stops at
// kMaxChunkBytes so a legacy 13-byte header (whose leading 0x00 sits
// exactly at offset 0xfff4 after a full chunk, one byte before its 0x64
// anchor) is left to the "no boundary -> read exactly kMaxChunkBytes" path
// rather than being mismatched one byte late.
bool FindChunkBoundary(const std::vector<uint8_t>& window, size_t* pos,
                       bool* is_end_of_page) {
  const size_t limit =
      std::min<size_t>(window.size(), static_cast<size_t>(kMaxChunkBytes));
  for (size_t p = 1; p <= limit; ++p) {
    if (p + 10 > window.size()) break;  // Too little left to match either.
    // End-of-page marker: 82 07 00 <pidx> 00 84 00 00 00 00. Only [+3]
    // (pidx) varies; the six zero bytes at [+2], [+4], [+6..9] plus the
    // 82/07/84 anchors are what keep this from matching JPEG entropy. A
    // looser 82/07/..84 test (three bytes) DID false-match mid-payload on
    // real ADF duplex data, cutting a page short.
    if (window[p] == 0x82 && window[p + 1] == 0x07 && window[p + 2] == 0x00 &&
        window[p + 4] == 0x00 && window[p + 5] == 0x84 &&
        window[p + 6] == 0x00 && window[p + 7] == 0x00 &&
        window[p + 8] == 0x00 && window[p + 9] == 0x00) {
      *pos = p;
      *is_end_of_page = true;
      return true;
    }
    // Next block header (12-byte shape): <type> 07 00 <pidx> 00 84 .. .. 00
    // 00, with type in {0x64, 0x40, 0x42}. The [+2] and [+4] zeros are
    // required for the same anti-false-match reason.
    if ((window[p] == 0x64 || window[p] == 0x40 || window[p] == 0x42) &&
        window[p + 1] == 0x07 && window[p + 2] == 0x00 &&
        window[p + 4] == 0x00 && window[p + 5] == 0x84 &&
        window[p + 8] == 0x00 && window[p + 9] == 0x00) {
      *pos = p;
      *is_end_of_page = false;
      return true;
    }
  }
  return false;
}

// Reads exactly ONE chunk's image bytes, given its already-parsed block
// header, appending them to *out. This is the per-chunk core of a JPEG
// payload's reassembly, factored out so a caller can read a single chunk
// at a time and route it (a whole page's worth of chunks assembled
// elsewhere) -- which the duplex de-interleaving loop in RunColorScan
// relies on, since a duplex feed multiplexes two pages' chunks and each
// chunk must go to its own page's buffer by the header's page index.
//
// The framing this reads was the key discovery of the multi-page task,
// found by scanning a real 1153x1684 color flatbed scan for its JPEG EOI
// and getting a file that decoded its header fine but failed full
// decompression with "Unsupported marker type" at libjpeg-turbo's actual
// entropy-decode step -- while the same bytes read back as a well-formed
// 345555-byte JPEG to `sips`/`file`. The bad marker sat at byte offset
// 65535, and dumping the raw stream around every 0x10000-ish boundary
// showed a repeating pattern: a second 12-byte block header (same anchors
// as the first) spliced into the stream every exactly 65524 bytes.
// BlockHeader::width's confirmed "JPEG-length sentinel" behavior from Task
// 6 (response.h) is this same mechanism seen from a single-block sample.
//
// Crucially, 0xfff4 (kMaxChunkBytes) is a "MORE DATA FOLLOWS" sentinel,
// NOT an exact per-block byte count. Real ADF hardware sends chunks that
// declare 0xfff4 yet carry fewer bytes: in this project's capture, ADF
// simplex page 1 has 52 chunks, and chunk 35's header declares 0xfff4
// while its real data is only 0xfa4c bytes -- the next valid block header
// sits at chunk35_data + 0xfa4c, not + 0xfff4. Trusting 0xfff4 as exact
// over-reads into the next header, desyncs the stream, and the following
// ReadBlockHeader fails with kProtocolError. (Single-page flatbed color
// happened to use only exact-0xfff4 chunks plus an honest short final
// chunk, so it worked and hid this until multi-page ADF exercised it.)
//
// So the chunk is read by its declared length's MEANING:
//   - width < kMaxChunkBytes: an honest length -- read exactly that many
//     bytes. (For a single-side page this is its final chunk, ending at
//     `ff d9`; for an interleaved page it may simply be a short chunk.)
//   - width == kMaxChunkBytes: a sentinel -- the chunk's real data ends at
//     the next boundary (FindChunkBoundary: the next block header OR the
//     end-of-page marker), capped at kMaxChunkBytes. Read up to that
//     boundary WITHOUT consuming it -- the caller's loop reads whatever
//     comes next (another header, or the 10-byte marker) itself. If no
//     boundary appears within kMaxChunkBytes, the chunk is genuinely full:
//     read exactly kMaxChunkBytes.
// Either way this consumes exactly one chunk's data and returns; it never
// reads the following header or marker, and it does NOT check for a JPEG
// EOI (that is a per-page finalization check the caller does once the page's
// end-of-page marker arrives).
Status ReadOneChunkBody(Framer* framer, const BlockHeader& header,
                        int timeout_ms, std::vector<uint8_t>* out) {
  if (header.width <= 0) return Status::kProtocolError;

  if (header.width != kMaxChunkBytes) {
    // Honest length: read exactly that many bytes.
    std::vector<uint8_t> chunk;
    const Status s =
        framer->ReadExact(static_cast<size_t>(header.width), timeout_ms, &chunk);
    if (s != Status::kOk) return s;
    out->insert(out->end(), chunk.begin(), chunk.end());
    return Status::kOk;
  }

  // Sentinel: the real chunk length isn't the header's 0xfff4. Look ahead
  // (without consuming) far enough to cover a full chunk plus the boundary
  // anchors that follow it, tolerating a short tail at the end of the
  // payload, and find where this chunk's data actually ends.
  std::vector<uint8_t> window;
  Status s = framer->PeekUpTo(static_cast<size_t>(kMaxChunkBytes) + 13,
                              timeout_ms, &window);
  if (s != Status::kOk) return s;

  size_t boundary = 0;
  bool is_end_of_page = false;
  if (FindChunkBoundary(window, &boundary, &is_end_of_page)) {
    std::vector<uint8_t> chunk;
    s = framer->ReadExact(boundary, timeout_ms, &chunk);
    if (s != Status::kOk) return s;
    out->insert(out->end(), chunk.begin(), chunk.end());
    return Status::kOk;  // Leave the boundary (header or marker) for the caller.
  }

  // No boundary within kMaxChunkBytes: a genuinely full chunk that
  // continues. Read exactly kMaxChunkBytes; the caller reads the next
  // header.
  std::vector<uint8_t> chunk;
  s = framer->ReadExact(static_cast<size_t>(kMaxChunkBytes), timeout_ms, &chunk);
  if (s != Status::kOk) return s;
  out->insert(out->end(), chunk.begin(), chunk.end());
  return Status::kOk;
}

// Runs the color (CGRAY/C=JPEG) readout for an ADF or flatbed job,
// de-interleaving the pages the device multiplexes on the wire, and
// appends one ScanResult per page to *out in page (end-of-page) order.
//
// A duplex ADF feed does NOT stream each page contiguously: it interleaves
// the two sides' chunks, tagging every block header with its 1-based page
// index (BlockHeader::page_index) and closing each page with its own
// 10-byte end-of-page marker carrying that same index (see
// reference/protocol-notes-adf-multipage.md and docs/PROTOCOL.md). So this
// keeps a per-page-index accumulator and, on each element of the stream,
// peeks the leading anchor to decide:
//   - a block header (leading byte != 0x82): read its ONE chunk body
//     (ReadOneChunkBody) and append to that page index's buffer;
//   - the end-of-page marker (`82 07 00 <pidx> 00 84 00 00 00 00`): finalize
//     page <pidx> -- EOI cross-check, DecodeJpeg, push a ScanResult -- then
//     peek the 2 bytes after it: `80 80` ends the job, anything else is the
//     next chunk's header (loop, do NOT consume it).
// Simplex (and flatbed, the single-page case) is the degenerate case where
// only one page index is ever active: the same loop handles it unchanged.
//
// The next element is classified by peeking its leading bytes, and the
// end-of-page marker MUST be recognized here BEFORE ReadBlockHeader is
// called: its `82 07 ... 84` bytes collide with the 12-byte header shape
// DetectHeaderLength keys on, so routing it through the header parser would
// consume the 2 bytes after it and desync the stream (see the marker note
// in reference/protocol-notes-adf-multipage.md). A color block header is
// `64 07 ..` (12-byte wire shape) or `00 64 07 ..` (legacy 13-byte shape);
// the marker is `82 07 ..`. Anything else at a decision point is a corrupt
// or unexpected element -- reported as a protocol error rather than fed to
// the header parser, which would misframe it.
constexpr int kBlockTypeColor = 0x64;

Status RunColorScan(Framer* framer, int timeout_ms,
                    std::vector<ScanResult>* out) {
  std::map<int, std::vector<uint8_t>> in_progress;  // page index -> JPEG bytes.
  for (;;) {
    std::vector<uint8_t> lead;
    Status s = framer->Peek(2, timeout_ms, &lead);
    if (s != Status::kOk) return s;

    const bool is_marker = lead[0] == 0x82;
    const bool is_header = lead[0] == kBlockTypeColor ||
                           (lead[0] == 0x00 && lead[1] == kBlockTypeColor);
    if (!is_marker && !is_header) return Status::kProtocolError;

    if (is_marker) {
      // End-of-page marker: 82 07 00 <pidx> 00 84 00 00 00 00 (10 bytes).
      std::vector<uint8_t> marker;
      s = framer->ReadExact(10, timeout_ms, &marker);
      if (s != Status::kOk) return s;
      if (marker[1] != 0x07 || marker[5] != 0x84) return Status::kProtocolError;
      const int pidx = marker[3];

      auto it = in_progress.find(pidx);
      if (it == in_progress.end()) return Status::kProtocolError;
      std::vector<uint8_t>& jpeg = it->second;
      // A well-formed page always ends at the JPEG EOI marker.
      if (jpeg.size() < 2 || jpeg[jpeg.size() - 2] != 0xff ||
          jpeg[jpeg.size() - 1] != 0xd9) {
        return Status::kProtocolError;
      }
      Image image;
      const Status decode_status = DecodeJpeg(jpeg.data(), jpeg.size(), &image);
      if (decode_status != Status::kOk) return decode_status;

      ScanResult page;
      page.format = PixelFormat::kRgb;
      page.width = image.width;
      page.height = image.height;
      page.data = std::move(jpeg);
      out->push_back(std::move(page));
      in_progress.erase(it);

      std::vector<uint8_t> tail;
      s = framer->Peek(2, timeout_ms, &tail);
      if (s != Status::kOk) return s;
      if (tail[0] == 0x80 && tail[1] == 0x80) {
        std::vector<uint8_t> consumed;
        s = framer->ReadExact(2, timeout_ms, &consumed);
        if (s != Status::kOk) return s;
        // Job done. Any page still accumulating never got its marker: a
        // truncated/desynced stream, not a clean finish.
        if (!in_progress.empty()) return Status::kProtocolError;
        return Status::kOk;
      }
      continue;  // `tail` is the next chunk's block header -- do not consume.
    }

    // Otherwise a block header: read it and route its one chunk by page
    // index.
    BlockHeader header;
    s = ReadBlockHeader(framer, timeout_ms, &header);
    if (s != Status::kOk) return s;
    s = ReadOneChunkBody(framer, header, timeout_ms,
                         &in_progress[header.page_index]);
    if (s != Status::kOk) return s;
  }
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

// BlockHeader::type values relevant to a C=RLENGTH scan (TEXT, ERRDIF,
// GRAY256; see response.h's doc comment on BlockHeader::type).
constexpr int kBlockTypeRaw = 0x40;
constexpr int kBlockTypeRlength = 0x42;

// Reads `height` per-row blocks for a C=RLENGTH scan (TEXT/ERRDIF/
// GRAY256) and returns their decoded pixel bytes concatenated row-major,
// `row_bytes` bytes per row.
//
// Each row's block header (already read once, as `first_header`, by the
// caller before it knew this was an RLENGTH scan -- mirroring how the
// color/gray branches below reuse their own first header read) declares
// that row's on-the-wire payload length in BlockHeader::width, and its
// on-the-wire shape in BlockHeader::type: kBlockTypeRlength (0x42) is a
// PackBits-compressed row, decoded here with DecodeRlengthRow;
// kBlockTypeRaw (0x40) is an uncompressed row of exactly `row_bytes`
// bytes -- observed for the large majority of rows in this project's own
// GRAY256 capture (see decode_rlength.h and the issue #4 report), so a
// real device response must not be rejected just for arriving raw. A
// header of any other type (e.g. the 0x82 end-of-page/status marker
// documented in reference/protocol-notes-modes.md) before `height` rows
// have been read is a protocol error rather than the well-formed image
// this function expects to reconstruct.
//
// Residual risk: this trusts each row's declared length even though this
// project's own captures show it can occasionally be wrong on real
// hardware (a single row, out of several thousand, with a declared
// length longer than the bytes actually sent for it before the next
// row's header began -- see the issue #4 report). Reading that many
// bytes then swallows the start of the next row's header, desynchronizing
// the rest of the image; there is no self-describing way to detect this
// from the header alone, so -- like ReadChunkedJpeg's analogous residual
// risk above -- it surfaces as a decode failure (a row that doesn't
// decompress to exactly `row_bytes`) or a timeout waiting for a header
// that isn't at the expected offset, not silent corruption.
Status ReadRlengthRows(Framer* framer, const BlockHeader& first_header,
                        int height, size_t row_bytes, int timeout_ms,
                        std::vector<uint8_t>* pixels) {
  pixels->clear();
  pixels->reserve(row_bytes * static_cast<size_t>(height));

  BlockHeader header = first_header;
  for (int row = 0; row < height; ++row) {
    if (row > 0) {
      const Status s = ReadBlockHeader(framer, timeout_ms, &header);
      if (s != Status::kOk) return s;
    }
    if (header.type != kBlockTypeRaw && header.type != kBlockTypeRlength) {
      return Status::kProtocolError;
    }
    // No header.width < 0 guard here: it's built from two uint8_t bytes
    // (see ParseBlockHeader in response.cpp), so it's always in [0, 65535].

    std::vector<uint8_t> payload;
    const Status s =
        framer->ReadExact(static_cast<size_t>(header.width), timeout_ms, &payload);
    if (s != Status::kOk) return s;

    if (header.type == kBlockTypeRaw) {
      if (payload.size() != row_bytes) return Status::kProtocolError;
      pixels->insert(pixels->end(), payload.begin(), payload.end());
    } else {
      std::vector<uint8_t> row_out(row_bytes);
      const Status ds = DecodeRlengthRow(payload.data(), payload.size(),
                                          row_out.data(), row_out.size());
      if (ds != Status::kOk) return ds;
      pixels->insert(pixels->end(), row_out.begin(), row_out.end());
    }
  }
  return Status::kOk;
}

}  // namespace

Status RunScan(Transport& transport, const Params& params,
                std::vector<ScanResult>* out) {
  if (out == nullptr) return Status::kProtocolError;
  out->clear();

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

  // Select source -- exactly one command, either ESC S FB (flatbed) or
  // ESC D ADF (document feeder), never both. The vendor driver's own
  // command stream (reference/streams/s0_out.bin) issues ESC D ADF *alone*
  // for its ADF scans (offsets 2044/2161, tests 7/8), with no preceding
  // ESC S FB; an earlier version here sent ESC S FB unconditionally and
  // then ESC D ADF, which left the device on the flatbed (a live ADF scan
  // came back at full flatbed dimensions). ESC S / ESC D each ack with a
  // short reply whose exact length isn't stable: the capture shows 2 bytes
  // for ESC S, but a live probe saw as little as 1, so drain it like the
  // ESC Q reply rather than assuming a fixed count.
  if (params.source == Source::kAdf) {
    status = send(EncodeSelectAdf());
    if (status != Status::kOk) return status;
    status = framer.DrainQuiet(kAckTimeoutMs, kDrainIdleTimeoutMs);
    if (status == Status::kTimeout) {
      // No response fixture for the ADF-empty case was ever captured (see
      // tests/fixtures/README.md: it's listed as a gap); the vendor
      // driver's own command stream simply stops right after this command
      // in the no-paper test. Heuristic, not confirmed: map a
      // source-select ack that doesn't arrive at all -- when every other
      // observed ack arrived in well under a second -- to kNoPaper rather
      // than a generic kTimeout.
      return Status::kNoPaper;
    }
    if (status != Status::kOk) return status;
  } else {
    status = send(EncodeSelectFlatbed());
    if (status != Status::kOk) return status;
    status = framer.DrainQuiet(kAckTimeoutMs, kDrainIdleTimeoutMs);
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

  // Execute. This runs once regardless of how many pages the device ends
  // up streaming back: a document-feeder scan negotiates and starts a
  // single job, and one ESC X makes the device feed and send the whole
  // stack over this same connection (see docs/PROTOCOL.md, "Multi-page
  // (ADF)"). Only the block/payload readout below loops per page.
  status = send(EncodeExecute(exec_params));
  if (status != Status::kOk) return status;

  // On any failure from here on, `out` must not be left holding a partial
  // page list (see scanner.h's doc comment on RunScan) -- clear it before
  // returning whatever error status was found.
  const auto fail = [&](Status s) {
    out->clear();
    return s;
  };

  // Color (CGRAY/C=JPEG) has its own de-interleaving readout: a duplex feed
  // multiplexes the two sides' chunks by page index, so it cannot use the
  // one-whole-page-at-a-time sequential loop below. RunColorScan handles
  // simplex, duplex, and single-page flatbed alike.
  if (params.mode == ScanMode::kColor) {
    status = RunColorScan(&framer, kScanTimeoutMs, out);
    if (status != Status::kOk) return fail(status);
    return Status::kOk;
  }

  // TODO(duplex gray/RLENGTH): the gray (GRAY64/C=NONE) and RLENGTH
  // (Black & White/Error Diffusion/True Gray) paths below read one whole
  // page at a time in EOP order. That is correct for simplex ADF and
  // flatbed, and matches every gray/RLENGTH sample captured (all
  // simplex/single-page -- see PROVENANCE.md), but duplex interleaving has
  // only ever been observed and captured for color. If the device also
  // interleaves duplex gray/RLENGTH chunks by page index, this loop would
  // desync; that case is uncaptured and out of scope here. Generalizing
  // these modes to the same page-index de-interleaving RunColorScan does
  // would require a real duplex gray/RLENGTH capture to confirm the framing.
  for (;;) {
    BlockHeader header;
    status = ReadBlockHeader(&framer, kScanTimeoutMs, &header);
    if (status != Status::kOk) return fail(status);

    ScanResult page;

    if (params.mode == ScanMode::kBlackWhite ||
        params.mode == ScanMode::kErrorDiffusion ||
        params.mode == ScanMode::kTrueGray) {
      const bool bitonal = params.mode != ScanMode::kTrueGray;
      const int width = exec_params.area.x1 - exec_params.area.x0;
      const int height = exec_params.area.y1 - exec_params.area.y0;
      if (width <= 0 || height <= 0) return fail(Status::kProtocolError);
      const size_t row_bytes = RlengthRowBytes(width, bitonal);

      std::vector<uint8_t> pixels;
      status = ReadRlengthRows(&framer, header, height, row_bytes,
                                kScanTimeoutMs, &pixels);
      if (status != Status::kOk) return fail(status);

      Image image;
      const Status decode_status =
          bitonal ? WrapBitonalImage(width, height, pixels, &image)
                  : DecodeGrayRaw(width, height, pixels.data(), pixels.size(),
                                  &image);
      if (decode_status != Status::kOk) return fail(decode_status);

      page.format = bitonal ? PixelFormat::kBitonal : PixelFormat::kGray;
      page.width = width;
      page.height = height;
      page.data = std::move(pixels);
    } else {
      // Gray: raw payload, exactly width * height bytes, no embedded
      // headers (see ReadRawGray). Width comes from this block's header
      // (confirmed reliable for a gray payload); height comes from the
      // requested area, since gray has no length field analogous to the
      // JPEG case to confirm it against.
      //
      // Residual risk: `height` here is what we *asked for*, not
      // something the device confirms back -- unlike color, where the
      // actual JPEG dimensions come from the decoded image itself. If the
      // device ever auto-crops a gray scan's delivered height short of
      // the requested area (as Task 6's report notes it does for some
      // same-offer color scans, trimming to detected paper edges),
      // ReadRawGray would block waiting for bytes that never arrive
      // rather than returning the shorter image, surfacing as
      // Status::kTimeout. Not observed in this task's live gray scans,
      // but not independently ruled out either.
      const int width = header.width;
      const int height = exec_params.area.y1 - exec_params.area.y0;
      if (width <= 0 || height <= 0) return fail(Status::kProtocolError);
      const size_t expected =
          static_cast<size_t>(width) * static_cast<size_t>(height);

      std::vector<uint8_t> raw;
      status = ReadRawGray(&framer, expected, kScanTimeoutMs, &raw);
      if (status != Status::kOk) return fail(status);

      Image image;
      const Status decode_status =
          DecodeGrayRaw(width, height, raw.data(), raw.size(), &image);
      if (decode_status != Status::kOk) return fail(decode_status);

      page.format = PixelFormat::kGray;
      page.width = width;
      page.height = height;
      page.data = std::move(raw);
    }

    out->push_back(std::move(page));

    // End-of-page marker: 82 07 00 <pidx> 00 84 00 00 00 00, exactly 10
    // bytes (see docs/PROTOCOL.md, "Multi-page (ADF)"). Read it as a fixed
    // count plus a 2-byte peek rather than routing it through
    // ReadBlockHeader, which would consume the 2 bytes right after it --
    // either the job-final `80 80` or the next page's block header -- and
    // desync the rest of the stream.
    std::vector<uint8_t> marker;
    status = framer.ReadExact(10, kScanTimeoutMs, &marker);
    if (status != Status::kOk) return fail(status);
    if (marker[0] != 0x82 || marker[1] != 0x07 || marker[5] != 0x84) {
      return fail(Status::kProtocolError);
    }

    std::vector<uint8_t> tail;
    status = framer.Peek(2, kScanTimeoutMs, &tail);
    if (status != Status::kOk) return fail(status);
    if (tail[0] == 0x80 && tail[1] == 0x80) {
      std::vector<uint8_t> consumed;
      status = framer.ReadExact(2, kScanTimeoutMs, &consumed);
      if (status != Status::kOk) return fail(status);
      break;  // Job-final terminator: no more pages.
    }
    // Otherwise `tail` is the next page's block header (e.g. `64 07` for
    // color) -- loop back to ReadBlockHeader without consuming it.
  }

  return Status::kOk;
}

}  // namespace brscan
