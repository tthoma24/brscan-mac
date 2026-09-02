#include "command.h"

#include <string>

namespace brscan {

namespace {

constexpr uint8_t kEsc = 0x1b;
constexpr uint8_t kLf = 0x0a;
constexpr uint8_t kTerminator = 0x80;

// Wraps `body` (already newline-terminated per line, or empty) as
// `0x1b <letter> 0x0a body 0x80`.
std::vector<uint8_t> Frame(char letter, const std::string& body) {
  std::vector<uint8_t> out;
  out.reserve(3 + body.size() + 1);
  out.push_back(kEsc);
  out.push_back(static_cast<uint8_t>(letter));
  out.push_back(kLf);
  out.insert(out.end(), body.begin(), body.end());
  out.push_back(kTerminator);
  return out;
}

const char* ModeToken(ScanMode mode) {
  return mode == ScanMode::kColor ? "CGRAY" : "GRAY64";
}

const char* DuplexToken(bool duplex) { return duplex ? "DUP" : "SIN"; }

}  // namespace

std::vector<uint8_t> EncodeQuery() { return Frame('Q', ""); }

std::vector<uint8_t> EncodeReset() { return {kEsc, 'R'}; }

std::vector<uint8_t> EncodeSelectFlatbed() { return Frame('S', "FB\n"); }

std::vector<uint8_t> EncodeSelectAdf() { return Frame('D', "ADF\n"); }

std::vector<uint8_t> EncodeInfo(int x_dpi, int y_dpi, ScanMode mode,
                                 bool duplex) {
  std::string body;
  body += "R=" + std::to_string(x_dpi) + "," + std::to_string(y_dpi) + "\n";
  body += "M=" + std::string(ModeToken(mode)) + "\n";
  body += "D=" + std::string(DuplexToken(duplex)) + "\n";
  return Frame('I', body);
}

std::vector<uint8_t> EncodeExecute(const Params& params) {
  const bool color = params.mode == ScanMode::kColor;

  std::string body;
  body += "B=" + std::to_string(params.brightness) + "\n";
  body += "N=" + std::to_string(params.contrast) + "\n";
  body += "M=" + std::string(ModeToken(params.mode)) + "\n";
  body += "C=" + std::string(color ? "JPEG" : "NONE") + "\n";
  if (color) {
    body += "J=MID\n";
  }
  body += "R=" + std::to_string(params.x_dpi) + "," +
          std::to_string(params.y_dpi) + "\n";
  body += "A=" + std::to_string(params.area.x0) + "," +
          std::to_string(params.area.y0) + "," +
          std::to_string(params.area.x1) + "," +
          std::to_string(params.area.y1) + "\n";
  body += "D=" + std::string(DuplexToken(params.duplex)) + "\n";
  body += "P=0\n";
  body += "E=1\n";
  body += "G=0\n";
  return Frame('X', body);
}

}  // namespace brscan
