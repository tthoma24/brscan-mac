#include "button_config.h"

#include <sstream>
#include <string>

namespace brscan::scand {

namespace {

constexpr uint8_t kFrameMagic = 0x30;
constexpr uint8_t kFrameReserved = 0x00;
constexpr size_t kFrameHeaderSize = 3;  // magic, length, reserved.

// Parses `s` as a base-10 non-negative int, requiring the whole string to
// be consumed. Returns 0 (this project's field default throughout
// ButtonConfig) for anything that doesn't parse cleanly, mirroring
// daemon/config.cpp's "an unparsable value leaves the field at its prior
// value" tolerance rather than crashing or throwing.
int ParseIntOr0(const std::string& s) {
  if (s.empty()) return 0;
  try {
    size_t consumed = 0;
    const int value = std::stoi(s, &consumed);
    if (consumed != s.size() || value < 0) return 0;
    return value;
  } catch (...) {
    return 0;
  }
}

// `1` -> true, anything else (including missing/empty) -> false, per the
// W=/G=/X= "0/1" fields' documented mapping.
bool ParseBool01(const std::string& s) { return s == "1"; }

// Applies one already-split, non-empty `key`/`value` line to `cfg`.
// Unrecognized keys are ignored outright (forward-compat with panel
// settings this parser doesn't know about); a recognized key with a value
// that doesn't fit its field's type leaves that field at its default,
// matching daemon/config.cpp's tolerant style. String fields (mode/paper/
// output_type/duplex_edge) store the token as-is with no validation, since
// mapping those tokens to this project's own enums is a later task's job.
void ApplyKey(ButtonConfig* cfg, const std::string& key,
              const std::string& value) {
  if (key == "F") {
    cfg->func = value;
  } else if (key == "D") {
    cfg->duplex = (value == "DUP");
  } else if (key == "E") {
    cfg->duplex_edge = value;
  } else if (key == "R") {
    cfg->dpi = ParseIntOr0(value);
  } else if (key == "M") {
    cfg->mode = value;
  } else if (key == "P") {
    cfg->paper = value;
  } else if (key == "A") {
    cfg->area_flag = ParseIntOr0(value);
  } else if (key == "T") {
    cfg->output_type = value;
  } else if (key == "W") {
    cfg->skip_blank = ParseBool01(value);
  } else if (key == "G") {
    cfg->remove_background = ParseBool01(value);
  } else if (key == "L") {
    cfg->remove_background_level = ParseIntOr0(value);
  } else if (key == "X") {
    cfg->high_speed = ParseBool01(value);
  }
  // Any other key: ignored (forward-compat).
}

// Parses the payload (everything after the 3-byte frame header: 0x0a-
// separated KEY=VALUE lines, trailing 0x0a included) into `cfg`. Always
// succeeds at the line-splitting level -- the only way this yields a
// malformed result is an empty/missing `F=`, which the caller checks for
// after this returns (see ParseButtonConfig()).
void ParsePayload(const std::string& payload, ButtonConfig* cfg) {
  std::istringstream lines(payload);
  std::string line;
  while (std::getline(lines, line, '\n')) {
    if (line.empty()) continue;  // Blank line, or the trailing separator.

    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;  // No '=': not a KEY=VALUE line.

    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (key.empty()) continue;

    ApplyKey(cfg, key, value);
  }
}

}  // namespace

std::optional<ButtonConfig> ParseButtonConfig(const uint8_t* data,
                                               size_t len) {
  if (data == nullptr || len < kFrameHeaderSize) return std::nullopt;
  if (data[0] != kFrameMagic) return std::nullopt;
  if (data[2] != kFrameReserved) return std::nullopt;

  const size_t actual_payload_len = len - kFrameHeaderSize;
  // byte[1] declares the payload length; reject rather than trust it
  // blindly, so a corrupt or truncated frame is rejected instead of
  // parsed on the wrong byte range.
  if (data[1] != actual_payload_len) return std::nullopt;

  const std::string payload(
      reinterpret_cast<const char*>(data + kFrameHeaderSize),
      actual_payload_len);

  ButtonConfig cfg;
  ParsePayload(payload, &cfg);

  // A config command always names its destination route; an empty (or
  // entirely missing) F= means the payload didn't actually describe a
  // scan-button config, so treat it as malformed rather than returning a
  // struct with a meaningless empty func.
  if (cfg.func.empty()) return std::nullopt;

  return cfg;
}

}  // namespace brscan::scand
