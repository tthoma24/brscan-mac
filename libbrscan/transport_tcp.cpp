#include "transport_tcp.h"

#include <cerrno>
#include <cstring>

#include <chrono>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace brscan {

namespace {

// Performs a connect() on `fd` bounded by `timeout_ms`: puts the socket in
// non-blocking mode, initiates the connect, and waits for it to become
// writable (or fail) via poll(). Restores blocking mode before returning on
// any path where the fd is still usable, since Read()/Write() expect a
// blocking socket.
Status ConnectBounded(int fd, const sockaddr* addr, socklen_t addrlen,
                       int timeout_ms) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) return Status::kIoError;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return Status::kIoError;

  if (connect(fd, addr, addrlen) != 0 && errno != EINPROGRESS) {
    return Status::kIoError;
  }

  // poll() can be interrupted by a signal (EINTR) before the deadline; retry
  // it against an absolute deadline computed once, the same pattern used in
  // Read(), so a retry only gets the time that genuinely remains rather than
  // restarting the full timeout on every signal.
  const auto deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
            .count();
    // Truncation toward zero means anything under 1ms remaining would
    // otherwise poll with a 0ms timeout (a valid, non-blocking poll) or,
    // worse in the analogous recv() case, disable the timeout outright.
    // Treat "less than a millisecond left" as timed out.
    if (remaining_ms <= 0) return Status::kTimeout;

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int rc = poll(&pfd, 1, static_cast<int>(remaining_ms));
    if (rc == 0) return Status::kTimeout;
    if (rc < 0) {
      if (errno == EINTR) continue;
      return Status::kIoError;
    }
    break;
  }

  int so_error = 0;
  socklen_t so_error_len = sizeof(so_error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0) {
    return Status::kIoError;
  }
  if (so_error != 0) return Status::kIoError;

  if (fcntl(fd, F_SETFL, flags) == -1) return Status::kIoError;
  return Status::kOk;
}

}  // namespace

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

    status = ConnectBounded(fd, ai->ai_addr, ai->ai_addrlen, connect_timeout_ms_);
    if (status == Status::kOk) {
      fd_ = fd;
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

  // SO_RCVTIMEO bounds a single recv() call, not the whole function: if
  // recv() is interrupted by a signal (EINTR) partway through the wait, the
  // timeout does not resume where it left off. Track an absolute deadline
  // computed once up front so a retry after EINTR only gets the time that
  // genuinely remains, instead of restarting the full timeout on every
  // signal (which could extend the effective wait indefinitely).
  const auto deadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(timeout_ms);

  for (;;) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
            .count();
    // duration_cast truncates toward zero, so 0 < remaining < 1ms would
    // otherwise compute remaining_ms == 0, and setsockopt(SO_RCVTIMEO, {0,0})
    // does not mean "expire immediately" — it means "no timeout" (BSD/POSIX
    // semantics), which would let the final recv() block indefinitely.
    // Treat "less than a millisecond left" as timed out before touching the
    // socket at all.
    if (remaining_ms <= 0) return Status::kTimeout;

    timeval tv{};
    tv.tv_sec = remaining_ms / 1000;
    tv.tv_usec = (remaining_ms % 1000) * 1000;
    if (setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
      return Status::kIoError;
    }

    const ssize_t n = recv(fd_, buf, cap, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return Status::kTimeout;
      return Status::kIoError;
    }
    if (n == 0) return Status::kIoError;  // Peer closed the connection.

    *out_len = static_cast<size_t>(n);
    return Status::kOk;
  }
}

}  // namespace brscan
