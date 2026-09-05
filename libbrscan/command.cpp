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
  switch (mode) {
    case ScanMode::kColor:
      return "CGRAY";
    case ScanMode::kGray:
      return "GRAY64";
    case ScanMode::kBlackWhite:
      return "TEXT";
    case ScanMode::kErrorDiffusion:
      return "ERRDIF";
    case ScanMode::kTrueGray:
      return "GRAY256";
  }
  return "GRAY64";  // unreachable
}

const char* DuplexToken(bool duplex) { return duplex ? "DUP" : "SIN"; }

// True for the modes that use the newer C=RLENGTH payload (see
// docs/PROTOCOL.md's "RLENGTH" section): these were captured from Brother
// iPrint&Scan rather than Image Capture, and that flow carries a distinct
// param set -- S=NORMAL_SCAN in both ESC I and ESC X, L=0 and E=0 (not
// E=1) in ESC X, and a different field order -- that reference/streams/
// modes_{text,errdif,gray256}_out.bin confirm the device accepts. See
// EncodeInfo and EncodeExecute below.
bool UsesRlength(ScanMode mode) {
  return mode == ScanMode::kBlackWhite || mode == ScanMode::kErrorDiffusion ||
         mode == ScanMode::kTrueGray;
}

}  // namespace

std::vector<uint8_t> EncodeQuery() { return Frame('Q', ""); }

std::vector<uint8_t> EncodeButtonQuery() { return Frame('K', ""); }

std::vector<uint8_t> EncodeReset() { return {kEsc, 'R'}; }

std::vector<uint8_t> EncodeSelectFlatbed() { return Frame('S', "FB\n"); }

std::vector<uint8_t> EncodeSelectAdf() { return Frame('D', "ADF\n"); }

std::vector<uint8_t> EncodeInfo(int x_dpi, int y_dpi, ScanMode mode,
                                 bool duplex, bool button_flow) {
  std::string body;
  body += "R=" + std::to_string(x_dpi) + "," + std::to_string(y_dpi) + "\n";
  body += "M=" + std::string(ModeToken(mode)) + "\n";
  body += "D=" + std::string(DuplexToken(duplex)) + "\n";
  // Confirmed in the ESC I bytes of reference/streams/modes_{text,errdif,
  // gray256}_out.bin: iPrint&Scan's RLENGTH flow also carries S=NORMAL_SCAN
  // here, appended after D= (the existing R,M,D order is unchanged). The
  // scan-button flow carries it for color too (button_flow; see
  // reference/protocol-notes-button-options.md), so the condition is
  // "RLENGTH mode OR button flow".
  if (UsesRlength(mode) || button_flow) body += "S=NORMAL_SCAN\n";
  return Frame('I', body);
}

std::vector<uint8_t> EncodeExecute(const Params& params) {
  const bool color = params.mode == ScanMode::kColor;
  const bool rlength = UsesRlength(params.mode);

  std::string body;
  if (rlength || params.button_flow) {
    // Field order and param set here replicate iPrint&Scan's ESC X for the
    // RLENGTH modes byte-for-byte (reference/streams/modes_{text,errdif,
    // gray256}_out.bin all share this exact shape, differing only in the
    // M= token): R,M,C,J,B,N,A,D,S,P,E,G,L -- a different order from the
    // CGRAY/GRAY64 branch below, and with S=NORMAL_SCAN, L=0, and E=0
    // (not E=1) that the older flow never sent. The device is documented
    // (docs/PROTOCOL.md) to accept either field order, so this is kept
    // byte-identical to the captured, known-working command rather than
    // merged into the older branch's ordering.
    //
    // The scan-button flow (params.button_flow) reuses this exact field
    // order for color too (reference/protocol-notes-button-options.md),
    // differing only in the C= token and the G=/L= remove-background values:
    //   - C=: RLENGTH for an RLENGTH mode; otherwise JPEG for color
    //     (button CGRAY) or NONE (button gray).
    //   - G=/L=: from params.remove_background / remove_background_level.
    // Both default so an RLENGTH (non-button) command is byte-identical to
    // before: rlength => C=RLENGTH, and G=0/L=0 from the field defaults.
    const char* c_token =
        rlength ? "RLENGTH" : (color ? "JPEG" : "NONE");
    body += "R=" + std::to_string(params.x_dpi) + "," +
            std::to_string(params.y_dpi) + "\n";
    body += "M=" + std::string(ModeToken(params.mode)) + "\n";
    body += "C=" + std::string(c_token) + "\n";
    body += "J=MID\n";
    body += "B=" + std::to_string(params.brightness) + "\n";
    body += "N=" + std::to_string(params.contrast) + "\n";
    body += "A=" + std::to_string(params.area.x0) + "," +
            std::to_string(params.area.y0) + "," +
            std::to_string(params.area.x1) + "," +
            std::to_string(params.area.y1) + "\n";
    body += "D=" + std::string(DuplexToken(params.duplex)) + "\n";
    body += "S=NORMAL_SCAN\n";
    body += "P=0\n";
    body += "E=0\n";
    body += "G=" + std::to_string(params.remove_background ? 1 : 0) + "\n";
    body += "L=" + std::to_string(params.remove_background_level) + "\n";
    return Frame('X', body);
  }

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
