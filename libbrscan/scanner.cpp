#include "brscan/scanner.h"

#include <jpeglib.h>

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

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

// The single-byte ack the device returns to an ESC D ADF source-select
// encodes feeder paper presence: 0x80 = a document is loaded (proceed),
// 0xc2 = the ADF is empty. Sourced from a black-box diff of
// reference/adf-loaded.pcap vs reference/adf-empty.pcap (see PROVENANCE.md);
// the two byte values carry no device identity. Only the ADF path inspects
// this -- the flatbed ESC S ack is not a paper-presence signal.
constexpr uint8_t kAdfAckLoaded = 0x80;
constexpr uint8_t kAdfAckEmpty = 0xc2;

// The device caps every payload block at this many bytes. A block header's
// trailing length field pins at this exact value (0xfff4) as a "more data
// follows" SENTINEL, NOT as an exact byte count: a chunk may declare
// 0xfff4 yet carry FEWER bytes before the next block header or the
// end-of-page marker (observed on real ADF hardware -- see the long
// comment above ReadChunkedJpeg). A length strictly below 0xfff4 is an
// honest final-chunk length.
constexpr int kMaxChunkBytes = 0xfff4;

// How many rows a streaming band carries at most, for the row-incremental
// (gray/bitonal) paths and the color decoder below. Small enough for a
// smooth progress bar, large enough not to flood the callback (an MCU row is
// 8-16 px, so 16 rows is one to two MCU rows -- see the task brief's band
// granularity note).
constexpr int kBandRows = 16;

// Fires one ScanBand through `on_band` (a no-op when it's empty). Returns
// Status::kOk to keep scanning, Status::kCancelled if the callback returned
// false (the caller then stops reading and unwinds).
Status EmitBand(const BandCallback& on_band, int page_index, PixelFormat format,
                int full_width, int full_height, int start_row, int num_rows,
                const uint8_t* data, size_t size) {
  if (!on_band) return Status::kOk;
  ScanBand band;
  band.page_index = page_index;
  band.format = format;
  band.full_width = full_width;
  band.full_height = full_height;
  band.start_row = start_row;
  band.num_rows = num_rows;
  band.data = data;
  band.size = size;
  return on_band(band) ? Status::kOk : Status::kCancelled;
}

// --- Incremental (streaming) color JPEG decode ----------------------------
//
// The non-streaming color path accumulates a page's whole JPEG and calls
// DecodeJpeg once at EOI. For live preview we instead decode as the device's
// chunks arrive, through a libjpeg-turbo SUSPENDING data source: its
// fill_input_buffer returns FALSE ("no more data right now") instead of
// blocking, so jpeg_read_header / jpeg_start_decompress / jpeg_read_scanlines
// return a suspension indication and we resume them once the next chunk has
// been fed (IncrementalJpegDecoder::Feed). Each resume drains whatever
// scanlines are now decodable and emits them as bands.
//
// Pixels are produced with the SAME settings DecodeJpeg's tjDecompress2 uses
// -- JCS_RGB output and the accurate (islow) IDCT that TJFLAG_ACCURATEDCT
// selects -- so a band's rows are byte-identical to the corresponding rows of
// the whole-page decode (proved by the streaming invariant tests). Recoverable
// warnings are swallowed exactly as DecodeJpeg tolerates TJERR_WARNING: real
// MFC-J6920DW ADF pages end a few MCUs short of their SOF height and libjpeg
// recovers by filling the tail. Only a fatal error_exit aborts the decode.

struct JpegErrorMgr {
  struct jpeg_error_mgr pub;
  jmp_buf jump;
};

void JpegErrorExit(j_common_ptr cinfo) {
  std::longjmp(reinterpret_cast<JpegErrorMgr*>(cinfo->err)->jump, 1);
}

// Swallow libjpeg's warning/trace output (its default prints to stderr).
void JpegEmitMessage(j_common_ptr, int) {}

class IncrementalJpegDecoder;

// Suspending source manager. `owner` links back to the decoder so a skip that
// runs past the currently-buffered bytes can be deferred to the next Feed.
struct SuspendSource {
  struct jpeg_source_mgr pub;
  IncrementalJpegDecoder* owner;
};

class IncrementalJpegDecoder {
 public:
  // Emits one band of decoded RGB scanlines: `rows` points at `num_rows`
  // scanlines of `full_width * 3` bytes each. Returns false to cancel.
  using EmitFn = std::function<bool(int start_row, int num_rows,
                                    const uint8_t* rows, size_t size,
                                    int full_width, int full_height)>;

  explicit IncrementalJpegDecoder(EmitFn emit) : emit_(std::move(emit)) {
    cinfo_.err = jpeg_std_error(&err_.pub);
    err_.pub.error_exit = &JpegErrorExit;
    err_.pub.emit_message = &JpegEmitMessage;
    if (setjmp(err_.jump)) {
      fatal_ = true;
      return;
    }
    jpeg_create_decompress(&cinfo_);
    created_ = true;
    src_.pub.init_source = [](j_decompress_ptr) {};
    src_.pub.fill_input_buffer = [](j_decompress_ptr) -> boolean {
      return FALSE;  // Suspend: the caller feeds more via Feed().
    };
    src_.pub.skip_input_data = &SkipInputData;
    src_.pub.resync_to_restart = jpeg_resync_to_restart;
    src_.pub.term_source = [](j_decompress_ptr) {};
    src_.pub.next_input_byte = nullptr;
    src_.pub.bytes_in_buffer = 0;
    src_.owner = this;
    cinfo_.src = &src_.pub;
  }

  ~IncrementalJpegDecoder() {
    if (created_) jpeg_destroy_decompress(&cinfo_);
  }

  IncrementalJpegDecoder(const IncrementalJpegDecoder&) = delete;
  IncrementalJpegDecoder& operator=(const IncrementalJpegDecoder&) = delete;

  bool fatal() const { return fatal_; }
  int width() const { return width_; }
  int height() const { return height_; }

  // Feeds `len` more JPEG bytes and decodes whatever is now available.
  // Returns kOk (progress made / awaiting more), kCancelled (the emit
  // callback asked to stop), or kProtocolError (fatal decode error).
  Status Feed(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
    return Pump(/*finishing=*/false);
  }

  // Called once the page's whole JPEG (through EOI) has been fed: drains any
  // remaining scanlines (libjpeg fills a short tail via a synthesized EOI)
  // and finishes.
  Status Finish() { return Pump(/*finishing=*/true); }

 private:
  static void SkipInputData(j_decompress_ptr cinfo, long num_bytes) {
    auto* src = reinterpret_cast<SuspendSource*>(cinfo->src);
    if (num_bytes <= 0) return;
    const size_t n = static_cast<size_t>(num_bytes);
    if (n <= src->pub.bytes_in_buffer) {
      src->pub.next_input_byte += n;
      src->pub.bytes_in_buffer -= n;
    } else {
      // The segment to skip isn't all buffered yet: consume what we have and
      // defer the rest to the next Feed (applied before the pointers are
      // re-armed).
      src->owner->skip_pending_ += n - src->pub.bytes_in_buffer;
      src->pub.next_input_byte += src->pub.bytes_in_buffer;
      src->pub.bytes_in_buffer = 0;
    }
  }

  // Records how much of buf_ libjpeg consumed so the next Pump can drop it.
  void Save() { consumed_ = buf_.size() - src_.pub.bytes_in_buffer; }

  Status Pump(bool finishing) {
    if (fatal_) return Status::kProtocolError;
    if (cancelled_) return Status::kCancelled;
    if (done_) return Status::kOk;

    if (setjmp(err_.jump)) {
      fatal_ = true;
      return Status::kProtocolError;
    }

    // Drop already-consumed bytes, apply any deferred skip, then arm the
    // source pointers at the unconsumed remainder.
    if (consumed_ > 0) {
      buf_.erase(buf_.begin(), buf_.begin() + consumed_);
      consumed_ = 0;
    }
    if (skip_pending_ > 0) {
      const size_t drop = std::min(skip_pending_, buf_.size());
      buf_.erase(buf_.begin(), buf_.begin() + drop);
      skip_pending_ -= drop;
      if (skip_pending_ > 0) return Status::kOk;  // Need more before skipping.
    }
    src_.pub.next_input_byte = buf_.data();
    src_.pub.bytes_in_buffer = buf_.size();

    if (!header_done_) {
      if (jpeg_read_header(&cinfo_, TRUE) == JPEG_SUSPENDED) {
        Save();
        return Status::kOk;
      }
      header_done_ = true;
      cinfo_.out_color_space = JCS_RGB;
      cinfo_.dct_method = JDCT_ISLOW;  // Match DecodeJpeg's TJFLAG_ACCURATEDCT.
    }
    if (!started_) {
      if (!jpeg_start_decompress(&cinfo_)) {
        Save();
        return Status::kOk;
      }
      started_ = true;
      width_ = static_cast<int>(cinfo_.output_width);
      height_ = static_cast<int>(cinfo_.output_height);
      stride_ = static_cast<size_t>(width_) *
                static_cast<size_t>(cinfo_.output_components);
      rows_.assign(stride_ * kBandRows, 0);
    }

    // Drain scanlines in row-groups, emitting a band each.
    while (cinfo_.output_scanline < cinfo_.output_height) {
      const int want = std::min<int>(
          kBandRows, static_cast<int>(cinfo_.output_height -
                                      cinfo_.output_scanline));
      JSAMPROW ptrs[kBandRows];
      for (int i = 0; i < want; ++i) ptrs[i] = rows_.data() + i * stride_;
      const int start = static_cast<int>(cinfo_.output_scanline);
      const JDIMENSION got =
          jpeg_read_scanlines(&cinfo_, ptrs, static_cast<JDIMENSION>(want));
      if (got == 0) {  // Suspended: no full scanline available yet.
        Save();
        return Status::kOk;
      }
      if (emit_ &&
          !emit_(start, static_cast<int>(got), rows_.data(),
                 static_cast<size_t>(got) * stride_, width_, height_)) {
        cancelled_ = true;
        return Status::kCancelled;
      }
    }

    // Every scanline produced. Finish (still needs the trailing EOI byte).
    if (!jpeg_finish_decompress(&cinfo_) && !finishing) {
      Save();
      return Status::kOk;
    }
    done_ = true;
    return Status::kOk;
  }

  EmitFn emit_;
  struct jpeg_decompress_struct cinfo_ {};
  JpegErrorMgr err_{};
  SuspendSource src_{};
  std::vector<uint8_t> buf_;   // Unconsumed JPEG bytes fed so far.
  std::vector<uint8_t> rows_;  // Scratch for one band's decoded scanlines.
  size_t consumed_ = 0;
  size_t skip_pending_ = 0;
  size_t stride_ = 0;
  int width_ = 0;
  int height_ = 0;
  bool created_ = false;
  bool header_done_ = false;
  bool started_ = false;
  bool done_ = false;
  bool fatal_ = false;
  bool cancelled_ = false;
};

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

  // Like DrainQuiet, but WITHOUT the mandatory first blocking Fill: it drains
  // whatever is already buffered plus any further chunks until a Fill() times
  // out, then discards it all. Used to finish consuming a reply whose leading
  // bytes a Peek has already pulled into the buffer -- there DrainQuiet's
  // initial Fill would instead read the *next* (or no) chunk and could return
  // kTimeout spuriously. Same heuristic-not-a-framing caveat as DrainQuiet.
  Status DrainBufferedQuiet(int idle_timeout_ms) {
    for (;;) {
      const Status s = Fill(idle_timeout_ms);
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

Status RunColorScan(Framer* framer, int timeout_ms, const BandCallback& on_band,
                    std::vector<ScanResult>* out) {
  std::map<int, std::vector<uint8_t>> in_progress;  // page index -> JPEG bytes.
  // Per-page suspending decoders, live only when streaming. A duplex feed
  // interleaves two pages' chunks, so each page index keeps its own decoder
  // state (matching the per-page JPEG accumulators above).
  std::map<int, std::unique_ptr<IncrementalJpegDecoder>> decoders;
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

      int page_width = 0;
      int page_height = 0;
      if (on_band) {
        // Streaming: the incremental decoder already produced every band; it
        // also carries the page's SOF dimensions (identical to what DecodeJpeg
        // reads). Draining it here also emits any trailing rows the device
        // left a few MCUs short of the SOF height.
        auto dit = decoders.find(pidx);
        if (dit == decoders.end()) return Status::kProtocolError;
        const Status fs = dit->second->Finish();
        if (fs != Status::kOk) return fs;  // kCancelled / kProtocolError.
        if (dit->second->fatal()) return Status::kProtocolError;
        page_width = dit->second->width();
        page_height = dit->second->height();
        decoders.erase(dit);
      } else {
        Image image;
        const Status decode_status =
            DecodeJpeg(jpeg.data(), jpeg.size(), &image);
        if (decode_status != Status::kOk) return decode_status;
        page_width = image.width;
        page_height = image.height;
      }

      ScanResult page;
      page.format = PixelFormat::kRgb;
      page.width = page_width;
      page.height = page_height;
      page.data = std::move(jpeg);
      out->push_back(std::move(page));
      in_progress.erase(it);

      // Peek the byte after the marker. A job-final terminator leads with
      // 0x80; a next chunk's block header with 0x64 (or 0x00 0x64). The
      // scan-button flow terminates a job with a SINGLE 0x80 and then
      // closes the connection; the vendor driver flow (persistent
      // connection) sends 0x80 0x80 before the next scan's data. A leading
      // 0x80 ends the job either way -- headers never lead with 0x80 and
      // the EOP marker leads with 0x82 -- so do NOT wait for a second 0x80:
      // Peek(2) blocks to the read timeout against the button flow's
      // single byte + connection close (the live 1d.5 failure).
      std::vector<uint8_t> tail;
      s = framer->Peek(1, timeout_ms, &tail);
      if (s != Status::kOk) return s;
      if (tail[0] == 0x80) {
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

    std::vector<uint8_t>& jpeg = in_progress[header.page_index];
    const size_t before = jpeg.size();
    s = ReadOneChunkBody(framer, header, timeout_ms, &jpeg);
    if (s != Status::kOk) return s;

    if (on_band) {
      // Feed only this chunk's fresh bytes into the page's decoder, emitting
      // whatever scanlines they complete. page_index is the 0-based device
      // page index (pidx - 1).
      auto& decoder = decoders[header.page_index];
      if (!decoder) {
        const int pidx = header.page_index;
        decoder = std::make_unique<IncrementalJpegDecoder>(
            [&on_band, pidx](int start_row, int num_rows, const uint8_t* rows,
                             size_t size, int full_width, int full_height) {
              return EmitBand(on_band, pidx - 1, PixelFormat::kRgb, full_width,
                              full_height, start_row, num_rows, rows,
                              size) == Status::kOk;
            });
      }
      s = decoder->Feed(jpeg.data() + before, jpeg.size() - before);
      if (s != Status::kOk) return s;  // kCancelled / kProtocolError.
    }
  }
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
                        const BandCallback& on_band, int page_index,
                        PixelFormat format, int width_px,
                        std::vector<uint8_t>* pixels) {
  pixels->clear();
  // Reserve the whole page up front so appending a row never reallocates: the
  // streaming bands below point directly into `pixels`, so their data must
  // stay put until the callback returns.
  pixels->reserve(row_bytes * static_cast<size_t>(height));

  BlockHeader header = first_header;
  int band_start = 0;  // First row of the band currently accumulating.
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

    // Flush a band every kBandRows decoded rows (and at the last row).
    if (on_band && ((row + 1) % kBandRows == 0 || row + 1 == height)) {
      const int num_rows = row + 1 - band_start;
      const Status es = EmitBand(
          on_band, page_index, format, width_px, height, band_start, num_rows,
          pixels->data() + static_cast<size_t>(band_start) * row_bytes,
          static_cast<size_t>(num_rows) * row_bytes);
      if (es != Status::kOk) return es;
      band_start = row + 1;
    }
  }
  return Status::kOk;
}

// Reads a raw gray (GRAY64/C=NONE) payload (width * height bytes, no embedded
// headers) in row-group increments, appending to *raw and emitting a band per
// group when streaming. Reserving the whole page keeps a band's bytes valid
// for the callback (see ReadRlengthRows). Reading in groups is byte-for-byte
// identical to a single ReadExact of the whole payload -- ReadExact loops
// internally either way.
//
// Unlike color, raw gray is NOT chunked with embedded block headers: a live
// probe found a 90000-byte gray scan read straight through with no header
// spliced in at the kMaxChunkBytes boundary (plain pixel data there, not an
// anchor). The mid-stream headers color uses are tied to the JPEG encoder's
// bounded output bursts; raw gray has no encoder and is one continuous byte
// stream, so the row-group split here is purely for band granularity.
Status ReadRawGrayStreaming(Framer* framer, int width, int height,
                            int timeout_ms, const BandCallback& on_band,
                            int page_index, std::vector<uint8_t>* raw) {
  raw->clear();
  raw->reserve(static_cast<size_t>(width) * static_cast<size_t>(height));
  const size_t row_bytes = static_cast<size_t>(width);
  for (int row = 0; row < height; row += kBandRows) {
    const int num_rows = std::min(kBandRows, height - row);
    std::vector<uint8_t> chunk;
    const Status s = framer->ReadExact(row_bytes * static_cast<size_t>(num_rows),
                                       timeout_ms, &chunk);
    if (s != Status::kOk) return s;
    raw->insert(raw->end(), chunk.begin(), chunk.end());
    if (on_band) {
      const Status es = EmitBand(
          on_band, page_index, PixelFormat::kGray, width, height, row, num_rows,
          raw->data() + static_cast<size_t>(row) * row_bytes,
          row_bytes * static_cast<size_t>(num_rows));
      if (es != Status::kOk) return es;
    }
  }
  return Status::kOk;
}

// Reads the ESC I offer reply -- [1-byte status][2-byte LE length][text][NUL]
// -- off the wire and returns the parsed Offer. Shared by RunScan and
// RunButtonScan: both negotiate with ESC I and MUST consume this reply in
// full before ESC X, whether or not they end up using its granted area.
//
// The status byte's value (0x00 in every sample seen, live or captured) is
// skipped, not validated -- nothing in either flow depends on it (a live
// probe and a re-read of reference/streams/s0_in.bin placed it as a separate
// leading byte here, not the tail of the preceding reply). The offer text
// carries a trailing NUL inside its length-prefixed frame; it's stripped
// before ParseOffer, which expects the CSV to end at its own terminating
// comma.
Status ReadOfferReply(Framer* framer, int timeout_ms, Offer* offer) {
  std::vector<uint8_t> offer_status;
  Status status = framer->ReadExact(1, timeout_ms, &offer_status);
  if (status != Status::kOk) return status;

  std::vector<uint8_t> len_bytes;
  status = framer->ReadExact(2, timeout_ms, &len_bytes);
  if (status != Status::kOk) return status;
  const size_t offer_len = static_cast<size_t>(len_bytes[0]) |
                           (static_cast<size_t>(len_bytes[1]) << 8);

  std::vector<uint8_t> offer_bytes;
  status = framer->ReadExact(offer_len, timeout_ms, &offer_bytes);
  if (status != Status::kOk) return status;
  if (!offer_bytes.empty() && offer_bytes.back() == 0) offer_bytes.pop_back();
  const std::string offer_csv(offer_bytes.begin(), offer_bytes.end());
  const auto parsed = ParseOffer(offer_csv);
  if (!parsed.has_value()) return Status::kProtocolError;
  *offer = *parsed;
  return Status::kOk;
}

// If exec_params.area is the zero value ({0,0,0,0}, "not set by the caller"),
// replaces it with the full area the ESC I offer just granted. Shared by both
// entry points; the scan-button flow normally supplies a concrete paper-table
// area, so this is the fallback path there rather than the usual one.
void ApplyOfferAreaFallback(const Offer& offer, Params* exec_params) {
  const Area& a = exec_params->area;
  if (a.x0 == 0 && a.y0 == 0 && a.x1 == 0 && a.y1 == 0) {
    exec_params->area = Area{0, 0, offer.width_px, offer.height_px};
  }
}

// Runs the block-header/payload readout that follows ESC X: one ScanResult
// per page, looped until the device's job-final terminator. Shared verbatim
// by RunScan and RunButtonScan -- the two differ only in their ESC K/ESC Q +
// source-select + ESC I/ESC X preamble; from ESC X onward the wire framing is
// identical (same block headers, same 10-byte end-of-page markers, same
// duplex de-interleaving), so this is the single readout implementation both
// call rather than a copy in each. It does NOT clear *out on error -- the
// caller owns the "out is empty on any non-kOk Status" contract and clears it
// before returning.
Status RunReadout(Framer* framer, const Params& exec_params, int timeout_ms,
                  const BandCallback& on_band, std::vector<ScanResult>* out) {
  // Color (CGRAY/C=JPEG) has its own de-interleaving readout: a duplex feed
  // multiplexes the two sides' chunks by page index, so it cannot use the
  // one-whole-page-at-a-time sequential loop below. RunColorScan handles
  // simplex, duplex, and single-page flatbed alike.
  if (exec_params.mode == ScanMode::kColor) {
    return RunColorScan(framer, timeout_ms, on_band, out);
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
    Status status = ReadBlockHeader(framer, timeout_ms, &header);
    if (status != Status::kOk) return status;

    ScanResult page;
    // 0-based page position; for the sequential (gray/RLENGTH) readout this
    // is also the index the finished page takes in `out` (see the duplex TODO
    // above -- these modes are simplex-only here).
    const int page_index = static_cast<int>(out->size());

    if (exec_params.mode == ScanMode::kBlackWhite ||
        exec_params.mode == ScanMode::kErrorDiffusion ||
        exec_params.mode == ScanMode::kTrueGray) {
      const bool bitonal = exec_params.mode != ScanMode::kTrueGray;
      const int width = exec_params.area.x1 - exec_params.area.x0;
      const int height = exec_params.area.y1 - exec_params.area.y0;
      if (width <= 0 || height <= 0) return Status::kProtocolError;
      const size_t row_bytes = RlengthRowBytes(width, bitonal);
      const PixelFormat format =
          bitonal ? PixelFormat::kBitonal : PixelFormat::kGray;

      std::vector<uint8_t> pixels;
      status = ReadRlengthRows(framer, header, height, row_bytes, timeout_ms,
                               on_band, page_index, format, width, &pixels);
      if (status != Status::kOk) return status;

      Image image;
      const Status decode_status =
          bitonal ? WrapBitonalImage(width, height, pixels, &image)
                  : DecodeGrayRaw(width, height, pixels.data(), pixels.size(),
                                  &image);
      if (decode_status != Status::kOk) return decode_status;

      page.format = bitonal ? PixelFormat::kBitonal : PixelFormat::kGray;
      page.width = width;
      page.height = height;
      page.data = std::move(pixels);
    } else {
      // Gray: raw payload, exactly width * height bytes, no embedded
      // headers (see ReadRawGrayStreaming). Width comes from this block's
      // header (confirmed reliable for a gray payload); height comes from the
      // requested area, since gray has no length field analogous to the
      // JPEG case to confirm it against.
      //
      // Residual risk: `height` here is what we *asked for*, not
      // something the device confirms back -- unlike color, where the
      // actual JPEG dimensions come from the decoded image itself. If the
      // device ever auto-crops a gray scan's delivered height short of
      // the requested area (as Task 6's report notes it does for some
      // same-offer color scans, trimming to detected paper edges),
      // ReadRawGrayStreaming would block waiting for bytes that never arrive
      // rather than returning the shorter image, surfacing as
      // Status::kTimeout. Not observed in this task's live gray scans,
      // but not independently ruled out either.
      const int width = header.width;
      const int height = exec_params.area.y1 - exec_params.area.y0;
      if (width <= 0 || height <= 0) return Status::kProtocolError;

      std::vector<uint8_t> raw;
      status = ReadRawGrayStreaming(framer, width, height, timeout_ms, on_band,
                                    page_index, &raw);
      if (status != Status::kOk) return status;

      Image image;
      const Status decode_status =
          DecodeGrayRaw(width, height, raw.data(), raw.size(), &image);
      if (decode_status != Status::kOk) return decode_status;

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
    status = framer->ReadExact(10, timeout_ms, &marker);
    if (status != Status::kOk) return status;
    if (marker[0] != 0x82 || marker[1] != 0x07 || marker[5] != 0x84) {
      return Status::kProtocolError;
    }

    // Peek the byte after the marker. A job-final terminator leads with
    // 0x80; a next page's block header with 0x64 (or 0x00 0x64). The
    // scan-button flow terminates a job with a SINGLE 0x80 and then
    // closes the connection; the vendor driver flow (persistent
    // connection) sends 0x80 0x80 before the next scan's data. A leading
    // 0x80 ends the job either way -- headers never lead with 0x80 and
    // the EOP marker leads with 0x82 -- so do NOT wait for a second 0x80:
    // Peek(2) blocks to the read timeout against the button flow's
    // single byte + connection close (the live BW/TIFF failure, same
    // root cause as the color path fixed in RunColorScan).
    std::vector<uint8_t> tail;
    status = framer->Peek(1, timeout_ms, &tail);
    if (status != Status::kOk) return status;
    if (tail[0] == 0x80) {
      break;  // Job-final terminator: no more pages.
    }
    // Otherwise `tail` is the next page's block header (e.g. `64 07` for
    // color) -- loop back to ReadBlockHeader without consuming it.
  }

  return Status::kOk;
}

}  // namespace

Status RunScan(Transport& transport, const Params& params,
                std::vector<ScanResult>* out) {
  return RunScan(transport, params, out, BandCallback{});
}

Status RunScan(Transport& transport, const Params& params,
                std::vector<ScanResult>* out, const BandCallback& on_band) {
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
    // Peek the ESC D ADF ack's first byte: it encodes feeder paper presence
    // (kAdfAckLoaded / kAdfAckEmpty; see the constants above and
    // PROVENANCE.md). An empty feeder must abandon the scan HERE, before ESC
    // I / ESC X -- otherwise the device (simplex) falls back to scanning the
    // glass or (duplex) sends nothing and times out. The flatbed ESC S ack
    // below carries no such signal, so only this ADF path checks it.
    std::vector<uint8_t> ack;
    status = framer.Peek(1, kAckTimeoutMs, &ack);
    if (status == Status::kTimeout) {
      // The ack never arrives at all -- observed when the vendor driver's own
      // command stream simply stops right after this command in a no-paper
      // test (see tests/fixtures/README.md, which lists the missing-ack case
      // as a gap). Every other observed ack arrived in well under a second,
      // so map this to kNoPaper rather than a generic kTimeout.
      return Status::kNoPaper;
    }
    if (status != Status::kOk) return status;
    if (ack[0] == kAdfAckEmpty) {
      // Feeder empty: stop before negotiating/executing. `out` is already
      // cleared above (the "out empty on any non-kOk Status" contract).
      return Status::kNoPaper;
    }
    // Document loaded (kAdfAckLoaded): drain the rest of the ack (its exact
    // length isn't stable -- 2 bytes in one capture, 1 live) and proceed.
    status = framer.DrainBufferedQuiet(kDrainIdleTimeoutMs);
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

  // Read and parse the ESC I offer reply (drained in full before ESC X;
  // see ReadOfferReply for the framing).
  Offer offer;
  status = ReadOfferReply(&framer, kAckTimeoutMs, &offer);
  if (status != Status::kOk) return status;

  // An all-zero Params::area means "not set by the caller": request the
  // full area the offer just granted.
  Params exec_params = params;
  ApplyOfferAreaFallback(offer, &exec_params);

  // Execute. This runs once regardless of how many pages the device ends
  // up streaming back: a document-feeder scan negotiates and starts a
  // single job, and one ESC X makes the device feed and send the whole
  // stack over this same connection (see docs/PROTOCOL.md, "Multi-page
  // (ADF)"). Only the block/payload readout below loops per page.
  status = send(EncodeExecute(exec_params));
  if (status != Status::kOk) return status;

  // From ESC X onward the readout is identical to the button flow's, so it
  // lives in RunReadout. On any failure, `out` must not be left holding a
  // partial page list (see scanner.h's doc comment on RunScan) -- clear it
  // before returning whatever error status was found. The one exception is
  // kCancelled (a caller's on_band returned false): keep the pages that
  // completed before the cancel, as the streaming overload's contract states.
  status = RunReadout(&framer, exec_params, kScanTimeoutMs, on_band, out);
  if (status == Status::kCancelled) return status;
  if (status != Status::kOk) {
    out->clear();
    return status;
  }
  return Status::kOk;
}

Status RunButtonScan(Transport& transport,
                     const ButtonParamsFn& params_from_config,
                     std::vector<ScanResult>* out) {
  if (out == nullptr) return Status::kProtocolError;
  out->clear();

  // Greeting -- identical to RunScan (Session::Open maps +OK 200 -> kOk,
  // -NG 401 -> kBusy, anything else -> kProtocolError).
  Status status = Session(&transport).Open();
  if (status != Status::kOk) return status;

  Framer framer(&transport);
  const auto send = [&](const std::vector<uint8_t>& bytes) {
    return transport.Write(bytes.data(), bytes.size());
  };

  // ESC K: the button-flow opener, in place of ESC Q. NO source-select
  // command follows (the button flow issues neither ESC S FB nor ESC D
  // ADF; see docs/BUTTON.md and reference/protocol-notes-button-options.md).
  status = send(EncodeButtonQuery());
  if (status != Status::kOk) return status;

  // Read the pushed config-command frame: a 3-byte header (0x30 <len> 0x00)
  // then <len> payload bytes. The full frame (header + payload) is handed to
  // the callback verbatim. Unlike the ESC Q capability block, this frame is
  // self-describing (its length byte), so it is read by exact count rather
  // than drained to quiet.
  std::vector<uint8_t> config;
  status = framer.ReadExact(3, kAckTimeoutMs, &config);
  if (status != Status::kOk) return status;
  if (config[0] != 0x30 || config[2] != 0x00) return Status::kProtocolError;
  const size_t payload_len = config[1];
  std::vector<uint8_t> payload;
  status = framer.ReadExact(payload_len, kAckTimeoutMs, &payload);
  if (status != Status::kOk) return status;
  config.insert(config.end(), payload.begin(), payload.end());

  // Ask the caller to turn the pushed config into concrete scan Params.
  // std::nullopt aborts the session as an unusable config (kProtocolError).
  std::optional<Params> maybe_params = params_from_config(config);
  if (!maybe_params.has_value()) return Status::kProtocolError;
  Params params = *maybe_params;
  // RunButtonScan *is* the button flow, so force the button ESC I/ESC X
  // shape regardless of what the callback left in Params::button_flow -- a
  // callback that forgot to set it must not silently fall back to the
  // normal (ESC Q-style) color ESC X.
  params.button_flow = true;

  // ESC I with S=NORMAL_SCAN (button_flow=true even for color). No
  // source-select preceded it.
  status = send(EncodeInfo(params.x_dpi, params.y_dpi, params.mode,
                           params.duplex, /*button_flow=*/true));
  if (status != Status::kOk) return status;

  // Drain and parse the offer reply. The button flow supplies a concrete
  // paper-table area in Params::area, so the offer's area is only a
  // fallback (an all-zero area) -- but its bytes MUST be consumed before
  // ESC X either way.
  Offer offer;
  status = ReadOfferReply(&framer, kAckTimeoutMs, &offer);
  if (status != Status::kOk) return status;

  Params exec_params = params;
  ApplyOfferAreaFallback(offer, &exec_params);

  // ESC X in the button variant (selected by exec_params.button_flow).
  status = send(EncodeExecute(exec_params));
  if (status != Status::kOk) return status;

  // The block/payload readout is identical to RunScan's from ESC X onward.
  // The scan-button flow has no band callback (its ICA/preview wiring is
  // separate), so it reads whole pages only.
  status = RunReadout(&framer, exec_params, kScanTimeoutMs, BandCallback{}, out);
  if (status != Status::kOk) {
    out->clear();
    return status;
  }
  return Status::kOk;
}

}  // namespace brscan
