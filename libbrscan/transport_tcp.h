#pragma once

#include <cstdint>
#include <string>

#include "transport.h"

namespace brscan {

// Blocking BSD-socket transport for the scanner's TCP control channel.
class TcpTransport : public Transport {
 public:
  TcpTransport(std::string host, uint16_t port)
      : host_(std::move(host)), port_(port) {}
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
  int fd_ = -1;
};

}  // namespace brscan
