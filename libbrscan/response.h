#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "brscan/types.h"

namespace brscan {

// Decoders for the scanner-to-host response bytes described in
// docs/PROTOCOL.md ("Responses (scanner to host)"). Pure functions only:
// no transport, no I/O. Reverse-engineered against the captured session in
// reference/streams/s0_in.bin (23 ESC I offers / per-scan data blocks); see
// .superpowers/sdd/i-want-to-create-bubbly-kahn/protocol-notes.md for the
// full survey.

// Parses an ESC I offer: the comma-terminated ASCII line
// `xdpi,ydpi,flag,f4,xmaxpx,f6,ymaxpx,` (note the trailing comma). Fields
// `flag`, `f4`, and `f6` have no confirmed meaning (f4/f6 are constant
// across dpi in every sample; `flag` is 2 for a per-scan grant and 1 for
// the capability probe reply) and are intentionally dropped rather than
// exposed with a guessed name.
//
// Returns std::nullopt if `csv` does not have exactly 7 comma-separated
// fields plus the trailing empty field from the final comma, or if any of
// the 7 fields is not a plain non-negative integer.
std::optional<Offer> ParseOffer(const std::string& csv);

// The per-scan binary block header that precedes each data payload
// (13 bytes in every sample captured). Only the fields below have a
// confirmed meaning; see the .cpp for the full anchor/field survey and
// what remains unconfirmed.
struct BlockHeader {
  // Bytes [11:13], little-endian u16.
  //
  // CONFIRMED for a raw grayscale (GRAY64/C=NONE) payload: the image
  // width in pixels. Verified against the 300 dpi gray scans in
  // reference/streams/s0_in.bin: header bytes `90 0d` -> 0x0d90 = 3472,
  // matching the raw payload's row width (raw byte count factors evenly
  // by 3472).
  //
  // NOT reliable for a JPEG (CGRAY/C=JPEG) payload: in every JPEG sample
  // this field instead holds the encoded byte length when it fits under
  // 0xfff4, or the sentinel value 0xfff4 (65524) when the JPEG is larger.
  // It is never the JPEG's pixel width (the JPEG's own SOF header carries
  // that; see DecodeJpeg). Callers must not use this field for a JPEG
  // payload's dimensions.
  int width;
};

// Parses the 13-byte block header. Validates two constant anchor bytes
// observed in every one of the 23 captured samples (offset 2 = 0x07,
// offset 6 = 0x84) before trusting the rest of the buffer as a header;
// returns std::nullopt if `len` is too short or the anchors don't match.
//
// Fields left unconfirmed (present in the bytes but not exposed here,
// because their meaning is not established -- see the .cpp for the data):
// offset 1 (0x40 in every gray sample, 0x64 in every color sample -- looks
// like a payload-type marker but only two payload types were observed);
// offsets 3-5 and 9-10 (constant `00 01 00` / `00 00` in every sample);
// offset 4 was 0x02 instead of 0x01 exactly once, on a region that also
// lacked a clean JPEG EOI in the capture, but that is a single data point
// and not confirmed as a status/cancel field.
std::optional<BlockHeader> ParseBlockHeader(const uint8_t* data, size_t len);

// Wraps a raw 8-bit grayscale payload (GRAY64 mode, C=NONE: the scanner
// sends already-decoded samples, no image codec involved) as an Image.
//
// `len` must equal `width * height` exactly. A cancelled or stalled gray
// scan produces a short byte count (see docs/PROTOCOL.md, "Cancellation");
// rather than crash or silently build a wrong-sized image, that case
// returns Status::kProtocolError so the caller can treat it as an
// incomplete scan.
Status DecodeGrayRaw(int width, int height, const uint8_t* data, size_t len,
                      Image* out);

}  // namespace brscan
