#pragma once

#include <cstddef>
#include <cstdint>

#include "brscan/types.h"
#include "transport.h"

namespace brscan {

// Decodes a complete JPEG buffer (SOI..EOI, the color/CGRAY scan payload
// described in docs/PROTOCOL.md) to an RGB Image, via libturbojpeg.
//
// `jpeg`/`len` must span exactly one baseline JPEG stream, as the scanner
// sends it: the whole payload in one clean buffer, not chunked. A
// truncated buffer (as produced by a cancelled scan with no clean EOI, see
// docs/PROTOCOL.md "Cancellation") fails to decode and returns
// Status::kProtocolError rather than crashing.
//
// Returns Status::kOk and fills `out` on success, Status::kProtocolError
// on any decode failure (malformed header, truncated data, or any other
// libturbojpeg error).
Status DecodeJpeg(const uint8_t* jpeg, size_t len, Image* out);

}  // namespace brscan
