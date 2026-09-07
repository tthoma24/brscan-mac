#include "log_safe.h"

#include <string>

#include <gtest/gtest.h>

namespace brscan::scand {
namespace {

TEST(LogSafeTest, NormalFieldPassesThroughUnchanged) {
  // A plain FUNC/USER/REGID value is all printable ASCII and must survive
  // verbatim so the log stays readable.
  EXPECT_EQ(LogSafe("OCR"), "OCR");
  EXPECT_EQ(LogSafe("Teddy's Mac-01"), "Teddy's Mac-01");
  EXPECT_EQ(LogSafe(""), "");
}

TEST(LogSafeTest, ControlAndEscapeBytesAreEscapedToOneSafeLine) {
  // The whole point: a spoofed field carrying a newline, carriage return, an
  // ANSI color escape, and DEL must not be able to forge a log line or drive
  // the terminal. Every such byte becomes a \xNN escape, and the result
  // contains no raw control byte.
  const std::string raw = "a\nb\r\x1b[31mc\x7f";
  const std::string safe = LogSafe(raw);
  EXPECT_EQ(safe, "a\\x0Ab\\x0D\\x1B[31mc\\x7F");
  EXPECT_EQ(safe.find('\n'), std::string::npos);
  EXPECT_EQ(safe.find('\r'), std::string::npos);
  EXPECT_EQ(safe.find('\x1b'), std::string::npos);
  EXPECT_EQ(safe.find('\x7f'), std::string::npos);
}

TEST(LogSafeTest, NonAsciiBytesAreEscaped) {
  // High-bit bytes (e.g. from a mangled multibyte field) are escaped too, so
  // the log stays plain ASCII.
  EXPECT_EQ(LogSafe(std::string("\xC3\xA9")), "\\xC3\\xA9");
}

TEST(LogSafeTest, OverLengthFieldIsTruncatedWithMarker) {
  // REGID can be as large as the datagram; cap the logged rendering and mark
  // that it was cut so a giant field can't flood a log line.
  const std::string raw(kLogSafeMaxChars + 50, 'x');
  const std::string safe = LogSafe(raw);
  EXPECT_EQ(safe, std::string(kLogSafeMaxChars, 'x') + "...");
}

TEST(LogSafeTest, TruncationCountsEscapeWidth) {
  // Escapes expand to four characters, so the cap is measured in emitted
  // characters, not input bytes: a run of control bytes truncates well before
  // kLogSafeMaxChars input bytes are consumed.
  const std::string raw(kLogSafeMaxChars, '\n');
  const std::string safe = LogSafe(raw);
  const int kEscapes = kLogSafeMaxChars / 4;  // each "\x0A" is 4 chars
  EXPECT_EQ(safe.size(), static_cast<size_t>(kEscapes * 4 + 3));  // + "..."
  EXPECT_TRUE(safe.rfind("...") == safe.size() - 3);
}

}  // namespace
}  // namespace brscan::scand
