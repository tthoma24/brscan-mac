#pragma once

#include <cstdint>
#include <string>

#include "transport.h"

namespace brscan {

// Blocking BSD-socket transport for the scanner's TCP control channel.
//
// Read()/Write() are ordinary blocking calls (Read() uses SO_RCVTIMEO to
// bound each call). Connect() is different: a plain blocking connect() has
// no timeout of its own, so an unreachable or powered-off device would hang
// the caller for the kernel's default TCP connect timeout (tens of
// seconds). Connect() instead bounds itself to kDefaultConnectTimeoutMs.
class TcpTransport : public Transport {
 public:
  // Default bound for Connect(). Exposed as a constructor default (rather
  // than hardcoded in the .cpp) so tests can substitute a short timeout
  // without waiting out the real one.
  static constexpr int kDefaultConnectTimeoutMs = 10000;

  explicit TcpTransport(std::string host, uint16_t port,
                         int connect_timeout_ms = kDefaultConnectTimeoutMs)
      : host_(std::move(host)),
        port_(port),
        connect_timeout_ms_(connect_timeout_ms) {}
  ~TcpTransport() override;

  TcpTransport(const TcpTransport&) = delete;
  TcpTransport& operator=(const TcpTransport&) = delete;

  Status Connect() override;
  void Disconnect() override;
  Status Write(const uint8_t* buf, size_t len) override;
  Status Read(uint8_t* buf, size_t cap, size_t* out_len,
              int timeout_ms) override;

 private:
  std::string host_;
  uint16_t port_;
  int connect_timeout_ms_;
  int fd_ = -1;
};

}  // namespace brscan
