#include "transport_tcp.h"

#include <cerrno>
#include <cstring>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace brscan {

TcpTransport::~TcpTransport() { Disconnect(); }

Status TcpTransport::Connect() {
  if (fd_ != -1) Disconnect();

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  const std::string port_str = std::to_string(port_);
  addrinfo* results = nullptr;
  const int rc = getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &results);
  if (rc != 0 || results == nullptr) return Status::kIoError;

  Status status = Status::kIoError;
  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      fd_ = fd;
      status = Status::kOk;
      break;
    }
    close(fd);
  }

  freeaddrinfo(results);
  return status;
}

void TcpTransport::Disconnect() {
  if (fd_ != -1) {
    close(fd_);
    fd_ = -1;
  }
}

Status TcpTransport::Write(const uint8_t* buf, size_t len) {
  if (fd_ == -1) return Status::kIoError;

  size_t sent = 0;
  while (sent < len) {
    const ssize_t n = send(fd_, buf + sent, len - sent, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::kIoError;
    }
    if (n == 0) return Status::kIoError;
    sent += static_cast<size_t>(n);
  }
  return Status::kOk;
}

Status TcpTransport::Read(uint8_t* buf, size_t cap, size_t* out_len,
                           int timeout_ms) {
  if (fd_ == -1) return Status::kIoError;

  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  if (setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
    return Status::kIoError;
  }

  const ssize_t n = recv(fd_, buf, cap, 0);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return Status::kTimeout;
    return Status::kIoError;
  }
  if (n == 0) return Status::kIoError;  // Peer closed the connection.

  *out_len = static_cast<size_t>(n);
  return Status::kOk;
}

}  // namespace brscan
