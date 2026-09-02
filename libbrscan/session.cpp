#include "session.h"

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
  size_t got = 0;
  const Status s = transport_->Read(buf, sizeof(buf), &got, kGreetingTimeoutMs);
  if (s != Status::kOk) return s;
  if (got >= 7 && std::memcmp(buf, "+OK 200", 7) == 0) return Status::kOk;
  // Only the one busy code observed during protocol capture (401) is
  // recognized here; this match is intentionally narrow, not an exhaustive
  // decoder for every possible -NG code.
  if (got >= 7 && std::memcmp(buf, "-NG 401", 7) == 0) return Status::kBusy;
  return Status::kProtocolError;
}

void Session::Close() { transport_->Disconnect(); }

}  // namespace brscan
