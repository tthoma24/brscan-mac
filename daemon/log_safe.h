#pragma once

#include <string>

// Escapes a wire-supplied field for safe logging. FUNC, USER, and REGID
// arrive off an untrusted UDP notification (see daemon/button_listener.h's
// ParseNotification, which validates the wire framing but not these fields'
// bytes) and are written verbatim to the daemon's stdout/stderr at many
// sites. A spoofed datagram -- the sender check is IP-based, so spoofable on
// a shared LAN -- could otherwise embed a newline, carriage return, or ANSI
// escape sequence in one of these fields and forge log lines or drive the
// operator's terminal. LogSafe neutralizes that before the value is logged.
//
// This is a distinct transform from handle_event.cpp's SanitizeForFilename
// (a strict filename allowlist that drops disallowed bytes): LogSafe keeps
// the field human-readable while making it a single, control-free line.
namespace brscan::scand {

// Returns a single-line, control-free rendering of `raw` safe to write to a
// log. Every byte < 0x20 (control characters, including '\n'/'\r'/'\t'/ESC),
// 0x7f (DEL), and every non-ASCII byte (>= 0x80) is replaced with a `\xNN`
// hex escape; all other printable ASCII passes through unchanged. The result
// is capped at kLogSafeMaxChars printable characters; anything beyond that is
// dropped and an ellipsis marker ("...") is appended so a truncation is
// visible in the log.
std::string LogSafe(const std::string& raw);

// Maximum number of characters LogSafe emits from the input before it
// truncates and appends the marker. Escapes count as the characters they
// expand to (e.g. a `\xNN` escape is four characters).
inline constexpr int kLogSafeMaxChars = 128;

}  // namespace brscan::scand
