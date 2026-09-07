#include "log_safe.h"

#include <cstdint>

namespace brscan::scand {

namespace {

// Appends the two-digit uppercase `\xNN` escape for `byte` to `out`.
void AppendHexEscape(std::string& out, unsigned char byte) {
  static constexpr char kHexDigits[] = "0123456789ABCDEF";
  out.push_back('\\');
  out.push_back('x');
  out.push_back(kHexDigits[byte >> 4]);
  out.push_back(kHexDigits[byte & 0x0f]);
}

}  // namespace

std::string LogSafe(const std::string& raw) {
  std::string out;
  // Reserve for the common all-printable case; the escaped path grows it.
  out.reserve(raw.size());
  int emitted = 0;
  for (const char c : raw) {
    const auto byte = static_cast<unsigned char>(c);
    // Printable ASCII (0x20 space .. 0x7e '~') passes through; everything
    // else -- C0 controls (incl. '\n'/'\r'/'\t'/ESC), DEL, and any non-ASCII
    // byte -- becomes a \xNN escape so the logged value can't contain a
    // newline or terminal escape sequence.
    const bool printable = byte >= 0x20 && byte <= 0x7e;
    const int width = printable ? 1 : 4;  // "c" vs "\xNN"
    if (emitted + width > kLogSafeMaxChars) {
      out += "...";
      break;
    }
    if (printable) {
      out.push_back(c);
    } else {
      AppendHexEscape(out, byte);
    }
    emitted += width;
  }
  return out;
}

}  // namespace brscan::scand
