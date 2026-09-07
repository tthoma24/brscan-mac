#include "brscan/session.h"

#include <chrono>
#include <cstring>

namespace brscan {

namespace {
// The greeting is a short ASCII line ("+OK 200\r\n" / "-NG 401\r\n"); 64
// bytes comfortably covers it with room to spare.
constexpr size_t kGreetingBufferSize = 64;
// How long to wait for the greeting the device sends unsolicited on
// connect.
constexpr int kGreetingTimeoutMs = 5000;
}  // namespace

Status Session::Open() {
  uint8_t buf[kGreetingBufferSize];
  size_t total = 0;
  // The device sends the greeting as one short ASCII line, but TCP may split
  // it across segments, so a single Read() can return fewer than the 7 bytes
  // the prefixes need. Accumulate across reads -- re-deriving the remaining
  // timeout each pass so the total wait stays bounded by kGreetingTimeoutMs --
  // until a full line ('\n') or at least 7 bytes are buffered. A genuine
  // timeout or EOF before then is the error, not a short first segment.
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kGreetingTimeoutMs);
  while (total < sizeof(buf)) {
    if (std::memchr(buf, '\n', total) != nullptr) break;
    if (total >= 7) break;  // Enough to match a prefix even without the '\n'.
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return Status::kTimeout;
    const int remaining_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count());
    size_t got = 0;
    const Status s =
        transport_->Read(buf + total, sizeof(buf) - total, &got, remaining_ms);
    if (s != Status::kOk) return s;
    if (got == 0) return Status::kProtocolError;  // EOF before a full greeting.
    total += got;
  }

  if (total >= 7 && std::memcmp(buf, "+OK 200", 7) == 0) return Status::kOk;
  // Only the one busy code observed during protocol capture (401) is
  // recognized here; this match is intentionally narrow, not an exhaustive
  // decoder for every possible -NG code.
  if (total >= 7 && std::memcmp(buf, "-NG 401", 7) == 0) return Status::kBusy;
  return Status::kProtocolError;
}

void Session::Close() { transport_->Disconnect(); }

}  // namespace brscan
