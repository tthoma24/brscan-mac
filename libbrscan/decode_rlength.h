#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "brscan/types.h"

namespace brscan {

// Decoder for the C=RLENGTH per-row payload used by the TEXT, ERRDIF, and
// GRAY256 scan modes (see docs/PROTOCOL.md's "RLENGTH" section). Pure
// functions only: no transport, no I/O -- scanner.cpp reads the per-row
// network blocks and calls these to turn each row's payload into decoded
// pixel bytes.
//
// RLENGTH is the classic Apple/TIFF PackBits algorithm. This was primarily
// reverse engineered from this project's own capture
// (reference/streams/modes_{text,errdif,gray256}_in.bin, git-ignored):
// decoding every compressed row in the TEXT capture with the algorithm
// below produces exactly 434 bytes (the 3472px-wide, 1-bit row width) for
// all 4913 rows; ERRDIF decodes 4896 of its 4897 compressed rows to the
// same 434 bytes (the one exception is a single row with a corrupted
// length field on the wire -- a capture/hardware anomaly, not a decode
// error: see PROVENANCE.md and the issue #4 report for the byte-level
// detail). The control-byte roles this implies are corroborated by
// `~/src/brscan`'s (`dmikushin/brscan`, GPLv2; see PROVENANCE.md)
// `libbrscandec/brother_scandec.c`, `FUN_001063f3`'s `nInDataComp == 3`
// branch.
//
// Control byte `c` (the first byte of a run):
//   c in [0x00, 0x7f]: literal run. Copy the next (c + 1) bytes from the
//     input verbatim.
//   c == 0x80: no-op. Produces no output and consumes no further input
//     byte. (Not exercised by any captured row in this project's
//     fixtures; see brother_scandec.c's `FUN_001063f3` for why this value
//     is treated as a no-op rather than anything else -- a decoder that
//     misparsed it would break on a row that used it.)
//   c in [0x81, 0xff]: repeat run. The single next byte from the input is
//     repeated (257 - c) times (2 to 128 repeats).

// Decodes one RLENGTH-compressed scanline. `out_len` is the caller-known,
// fixed decompressed row width in bytes (see RowBytes below); decoding
// must consume `in` exactly and produce exactly `out_len` bytes for this
// to succeed. Returns Status::kProtocolError for any of: a control byte
// whose run would read past the end of `in`, a literal or repeat run that
// would write past `out_len`, or input left over (or exhausted early)
// so the output length ends up short of `out_len`. Never reads or writes
// outside `in`/`out`'s given bounds.
Status DecodeRlengthRow(const uint8_t* in, size_t in_len, uint8_t* out,
                         size_t out_len);

// The fixed row byte width for a given pixel width and pixel depth:
// (width_px + 7) / 8 for a 1-bit-per-pixel row (TEXT/ERRDIF), or width_px
// itself for an 8-bit-per-pixel row (GRAY256).
size_t RlengthRowBytes(int width_px, bool bitonal);

// Wraps `packed_rows` -- row-major, already-decoded 1-bit-per-pixel
// samples (RlengthRowBytes(width_px, /*bitonal=*/true) bytes per row; see
// PixelFormat::kBitonal in types.h for the bit convention) -- as an
// Image. `packed_rows.size()` must equal RlengthRowBytes(width_px, true)
// * height exactly, matching DecodeGrayRaw's contract in response.h for
// the analogous 8-bit case.
Status WrapBitonalImage(int width_px, int height,
                         std::vector<uint8_t> packed_rows, Image* out);

}  // namespace brscan
