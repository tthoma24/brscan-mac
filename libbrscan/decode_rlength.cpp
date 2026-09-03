#include "decode_rlength.h"

namespace brscan {

Status DecodeRlengthRow(const uint8_t* in, size_t in_len, uint8_t* out,
                         size_t out_len) {
  if (in == nullptr || out == nullptr) return Status::kProtocolError;

  size_t ii = 0;
  size_t oi = 0;
  while (ii < in_len) {
    const uint8_t ctrl = in[ii++];
    if (ctrl < 0x80) {
      // Literal run: copy the next (ctrl + 1) bytes verbatim.
      const size_t count = static_cast<size_t>(ctrl) + 1;
      if (ii + count > in_len || oi + count > out_len) {
        return Status::kProtocolError;
      }
      for (size_t k = 0; k < count; ++k) out[oi + k] = in[ii + k];
      ii += count;
      oi += count;
    } else if (ctrl == 0x80) {
      // No-op: no output, no value byte consumed.
      continue;
    } else {
      // Repeat run: the next single byte repeated (257 - ctrl) times.
      const size_t count = 257u - static_cast<size_t>(ctrl);
      if (ii >= in_len || oi + count > out_len) return Status::kProtocolError;
      const uint8_t value = in[ii++];
      for (size_t k = 0; k < count; ++k) out[oi + k] = value;
      oi += count;
    }
  }

  // A well-formed row consumes its whole compressed payload and produces
  // exactly the fixed row width; anything less is a truncated/malformed
  // row (see docs/PROTOCOL.md, "Cancellation", for why a scan can end
  // short) rather than a valid decode.
  if (oi != out_len) return Status::kProtocolError;
  return Status::kOk;
}

size_t RlengthRowBytes(int width_px, bool bitonal) {
  if (width_px <= 0) return 0;
  if (bitonal) return (static_cast<size_t>(width_px) + 7) / 8;
  return static_cast<size_t>(width_px);
}

Status WrapBitonalImage(int width_px, int height,
                         std::vector<uint8_t> packed_rows, Image* out) {
  if (width_px <= 0 || height <= 0 || out == nullptr) {
    return Status::kProtocolError;
  }
  const size_t expected = RlengthRowBytes(width_px, /*bitonal=*/true) *
                           static_cast<size_t>(height);
  if (packed_rows.size() != expected) return Status::kProtocolError;

  out->width = width_px;
  out->height = height;
  out->format = PixelFormat::kBitonal;
  out->pixels = std::move(packed_rows);
  return Status::kOk;
}

}  // namespace brscan
