#pragma once

#include <cstdint>
#include <optional>
#include <string>

// A parser for the Brother scan-button *config command*: the printer's
// `0x30 <len> 0x00`-framed KEY=VALUE push that follows the host's `ESC K`
// (`1b 4b`) and carries the LCD's current scan settings (destination,
// mode, dpi, paper, output format, and the panel toggles) for the button
// press about to happen. See docs/BUTTON.md's "Config command" section for
// the wire format and full field vocabulary, and PROVENANCE.md for the
// capture this was decoded from.
//
// This parser is deliberately a faithful decode only: paper (`P=`) and
// output-type (`T=`) tokens are kept as raw strings rather than mapped to
// this project's scan-area or OutputFormat types -- those mappings are
// later Plan 1d tasks, and keeping this parser decoupled from them lets it
// be tested (and trusted) on its own.
namespace brscan::scand {

// A parsed button config command. Every field mirrors one `KEY=VALUE` line
// in the payload (see docs/BUTTON.md); see each field's comment for its
// source key and value mapping. Fields default to the zero-ish value a
// config command that omits that key implies (matching daemon/config.cpp's
// tolerant style): an absent key never fails the parse, it just leaves the
// corresponding field at this default.
struct ButtonConfig {
  std::string func;     // F=  FILE | IMAGE | OCR | EMAIL. Never empty on a
                         // successful parse -- see ParseButtonConfig().
  bool duplex = false;   // D=  SIN -> false, DUP -> true.
  std::string duplex_edge;  // E=  LON | SHO (raw token; only meaningful
                             // when duplex is true).
  int dpi = 0;            // R=  single resolution value (this command only
                           // ever carries one; ESC I/X's offer/request use
                           // an "x,y" pair instead -- unrelated to this).
  std::string mode;      // M=  CGRAY (color) | TEXT (black & white), raw
                          // token.
  std::string paper;     // P=  LETTER | LEGAL | A4 | LEDGER | A3 | A5 |
                          // EXECUTIVE | PHOTO | BCARD, raw token. Mapped to
                          // a scan area by a later task, not here.
  int area_flag = 0;      // A=  observed always 0 (auto-area); the real
                           // scan area is computed downstream.
  std::string output_type;  // T=  PDF(Image) | MULTI-TIFF | JPEG | TXT |
                             // HTML | RTF, raw token (kept verbatim,
                             // parens and hyphen included). Mapped to
                             // OutputFormat by a later task, not here.
  bool skip_blank = false;   // W=  0/1.
  bool remove_background = false;  // G=  0/1. OCR config commands omit
                                    // this key entirely; its absence keeps
                                    // this false, matching that omission.
  int remove_background_level = 0;  // L=  64 (Low) | 128 (Med) | 192
                                     // (High); present only when G=1,
                                     // otherwise absent and left at 0.
  bool high_speed = false;  // X=  0/1.
};

// Parses a config-command frame (`data`, `len` bytes: the exact bytes
// received off the wire) into a ButtonConfig. The frame is
// `{0x30, <payload_len>, 0x00, <payload...>}`, where `<payload_len>` (a
// single unsigned byte, byte[1]) must equal the number of payload bytes
// that actually follow byte[2] -- see docs/BUTTON.md. Returns
// std::nullopt if the frame itself is malformed: fewer than 3 bytes,
// data[0] != 0x30, data[2] != 0x00, or `<payload_len>` doesn't match the
// actual remaining byte count; and never reads outside [data, data + len).
//
// Given a well-framed payload, KEY=VALUE lines (0x0a-separated, matching
// daemon/config.cpp's tolerant style) are parsed leniently: an unknown key
// is ignored (forward-compat with panel settings this parser doesn't know
// about yet); a missing key leaves its field at the default above; an
// unparsable value for a string field is stored as-is (there's nothing to
// validate it against here); an unparsable value for a bool/int field
// leaves that field at its default rather than crashing. The one
// exception is `F=`: a config command always names its destination route,
// so an empty or missing `F` is treated as a malformed payload and this
// still returns std::nullopt.
std::optional<ButtonConfig> ParseButtonConfig(const uint8_t* data, size_t len);

}  // namespace brscan::scand
