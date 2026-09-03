#include "response.h"

#include <cctype>
#include <cstdlib>

namespace brscan {
namespace {

// Splits `s` on ',' with no field collapsing, so a trailing separator
// yields a trailing empty field (e.g. "a,b," -> {"a", "b", ""}). That
// trailing empty field is how ParseOffer recognizes the offer's
// terminating comma.
std::vector<std::string> SplitOnComma(const std::string& s) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (true) {
    const size_t comma = s.find(',', start);
    if (comma == std::string::npos) {
      fields.push_back(s.substr(start));
      break;
    }
    fields.push_back(s.substr(start, comma - start));
    start = comma + 1;
  }
  return fields;
}

// Parses `field` as a plain non-negative decimal integer (no sign, no
// whitespace, no leading garbage). Returns false if `field` is empty or
// contains anything else.
bool ParseUnsignedField(const std::string& field, int* out) {
  if (field.empty()) return false;
  for (char c : field) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  char* end = nullptr;
  const long value = std::strtol(field.c_str(), &end, 10);
  if (end != field.c_str() + field.size()) return false;
  *out = static_cast<int>(value);
  return true;
}

}  // namespace

std::optional<Offer> ParseOffer(const std::string& csv) {
  // Expect 7 numeric fields followed by an empty trailing field produced
  // by the offer's terminating comma, i.e. exactly 8 entries with the
  // last one empty: `xdpi,ydpi,flag,f4,xmaxpx,f6,ymaxpx,`.
  const std::vector<std::string> fields = SplitOnComma(csv);
  if (fields.size() != 8 || !fields.back().empty()) return std::nullopt;

  int values[7];
  for (int i = 0; i < 7; ++i) {
    if (!ParseUnsignedField(fields[i], &values[i])) return std::nullopt;
  }

  Offer offer;
  offer.x_dpi = values[0];
  offer.y_dpi = values[1];
  // values[2] (flag), values[3] (f4), and values[5] (f6) are dropped; see
  // the doc comment on Offer / ParseOffer in response.h.
  offer.width_px = values[4];
  offer.height_px = values[6];
  return offer;
}

namespace {
constexpr size_t kBlockHeaderLen = 13;
}  // namespace

std::optional<BlockHeader> ParseBlockHeader(const uint8_t* data, size_t len) {
  if (data == nullptr || len < kBlockHeaderLen) return std::nullopt;

  // Anchor bytes confirmed constant across all 23 per-scan blocks in
  // reference/streams/s0_in.bin, both gray and color. Reject anything
  // that doesn't match rather than risk misparsing unrelated bytes (e.g.
  // a resync after a stalled/cancelled scan) as a well-formed header.
  if (data[2] != 0x07 || data[6] != 0x84) return std::nullopt;

  BlockHeader header;
  // Little-endian u16 at bytes [11:13]. See the doc comment on
  // BlockHeader::width in response.h for what this is confirmed (gray
  // payload width) and not confirmed (JPEG payload length/sentinel) to
  // mean.
  header.width = static_cast<int>(data[11]) | (static_cast<int>(data[12]) << 8);
  // Byte 1: the payload-type marker. See the doc comment on
  // BlockHeader::type in response.h.
  header.type = static_cast<int>(data[1]);
  return header;
}

Status DecodeGrayRaw(int width, int height, const uint8_t* data, size_t len,
                      Image* out) {
  if (width <= 0 || height <= 0 || data == nullptr || out == nullptr) {
    return Status::kProtocolError;
  }
  const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (len != expected) {
    // A cancelled or stalled gray scan (see docs/PROTOCOL.md,
    // "Cancellation") ends with fewer bytes than width * height. Report
    // it as incomplete rather than building a wrong-sized Image.
    return Status::kProtocolError;
  }

  out->width = width;
  out->height = height;
  out->format = PixelFormat::kGray;
  out->pixels.assign(data, data + len);
  return Status::kOk;
}

}  // namespace brscan
