#include "button_listener.h"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <unordered_map>

#include <chrono>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace brscan::scand {

namespace {

constexpr size_t kHeaderSize = 4;
constexpr uint8_t kHeaderConstantByte = 0x30;
// See the header-byte-2 comment on ParseNotification(): the real capture's
// value is payload.size() + kHeaderSize + 1.
constexpr size_t kHeaderLengthOverhead = kHeaderSize + 1;

// Parses `s` as a base-10 non-negative integer, requiring the whole string
// to be consumed. Rejects empty strings, so a KEY= with no value cannot
// silently become 0.
bool ParseInt(const std::string& s, int* out) {
  if (s.empty()) return false;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), *out);
  return ec == std::errc() && ptr == s.data() + s.size();
}

// Splits `host` ("<ip>:<port>") into its parts. Uses the *last* ':' so a
// bare-numeric-port suffix parses even if a future IPv6 host contained
// colons of its own.
bool ParseHost(const std::string& host, std::string* ip, uint16_t* port) {
  const size_t colon = host.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon == host.size() - 1) {
    return false;
  }
  int port_value = 0;
  if (!ParseInt(host.substr(colon + 1), &port_value)) return false;
  if (port_value < 0 || port_value > 65535) return false;

  *ip = host.substr(0, colon);
  *port = static_cast<uint16_t>(port_value);
  return true;
}

// Splits a KEY=VALUE;KEY=VALUE;... payload into a map, unquoting a
// double-quoted value (only USER uses this in practice). Returns false for
// any semicolon-delimited token that isn't KEY=VALUE -- in particular, a
// notification truncated mid-key ends in a token with no '=' at all.
bool SplitFields(const std::string& payload,
                  std::unordered_map<std::string, std::string>* fields) {
  size_t pos = 0;
  while (pos < payload.size()) {
    const size_t semi = payload.find(';', pos);
    const std::string token = payload.substr(
        pos, semi == std::string::npos ? std::string::npos : semi - pos);
    pos = semi == std::string::npos ? payload.size() : semi + 1;
    if (token.empty()) continue;

    const size_t eq = token.find('=');
    if (eq == std::string::npos) return false;

    std::string key = token.substr(0, eq);
    std::string value = token.substr(eq + 1);
    if (!value.empty() && value.front() == '"') {
      // A quoted value must have a matching closing quote; a value that
      // starts with '"' but never closes (e.g. a datagram truncated
      // mid-USER) is malformed, not a value that happens to start with a
      // literal quote character.
      if (value.size() < 2 || value.back() != '"') return false;
      value = value.substr(1, value.size() - 2);
    }
    (*fields)[key] = std::move(value);
  }
  return true;
}

}  // namespace

std::optional<ButtonEvent> ParseNotification(const uint8_t* data, size_t len) {
  if (data == nullptr || len < kHeaderSize) return std::nullopt;
  if (data[3] != kHeaderConstantByte) return std::nullopt;

  const uint8_t header_len = data[2];
  if (header_len < kHeaderLengthOverhead) return std::nullopt;
  const size_t expected_payload_len = header_len - kHeaderLengthOverhead;
  const size_t actual_payload_len = len - kHeaderSize;
  // Reject rather than clamp: a header claiming a different payload length
  // than what actually arrived means the datagram is corrupt or truncated,
  // not that we should guess which bytes are real (see the ParseNotification
  // comment in button_listener.h for the length-byte formula this checks).
  if (expected_payload_len != actual_payload_len) return std::nullopt;

  const std::string payload(
      reinterpret_cast<const char*>(data + kHeaderSize), actual_payload_len);

  std::unordered_map<std::string, std::string> fields;
  if (!SplitFields(payload, &fields)) return std::nullopt;

  const auto type_it = fields.find("TYPE");
  if (type_it == fields.end() || type_it->second != "BR") return std::nullopt;
  const auto button_it = fields.find("BUTTON");
  if (button_it == fields.end() || button_it->second != "SCAN") {
    return std::nullopt;
  }

  ButtonEvent event;

  const auto func_it = fields.find("FUNC");
  if (func_it == fields.end() || func_it->second.empty()) return std::nullopt;
  event.func = func_it->second;

  const auto user_it = fields.find("USER");
  if (user_it == fields.end()) return std::nullopt;
  event.user = user_it->second;

  const auto host_it = fields.find("HOST");
  if (host_it == fields.end()) return std::nullopt;
  if (!ParseHost(host_it->second, &event.host_ip, &event.host_port)) {
    return std::nullopt;
  }

  const auto appnum_it = fields.find("APPNUM");
  if (appnum_it == fields.end() || !ParseInt(appnum_it->second, &event.appnum)) {
    return std::nullopt;
  }

  const auto regid_it = fields.find("REGID");
  if (regid_it == fields.end() || regid_it->second.empty()) return std::nullopt;
  event.regid = regid_it->second;

  const auto seq_it = fields.find("SEQ");
  if (seq_it == fields.end() || !ParseInt(seq_it->second, &event.seq)) {
    return std::nullopt;
  }

  return event;
}

ButtonListener::ButtonListener(uint16_t port) : port_(port) {}

ButtonListener::~ButtonListener() {
  if (fd_ != -1) close(fd_);
}

Status ButtonListener::Bind() {
  // Without this guard, a second Bind() call would overwrite fd_ with a
  // new socket and leak the previous one (it's never close()d).
  if (fd_ != -1) return Status::kIoError;

  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return Status::kIoError;

  const int reuse = 1;
  // Best-effort: a failure here doesn't prevent binding, it just risks a
  // stuck TIME_WAIT-style failure on rapid rebind, so don't fail Bind()
  // over it.
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return Status::kIoError;
  }

  if (port_ == 0) {
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) ==
        0) {
      port_ = ntohs(bound.sin_port);
    }
  }

  fd_ = fd;
  return Status::kOk;
}

Status ButtonListener::Receive(int timeout_ms, ButtonEvent* event,
                                std::vector<uint8_t>* raw,
                                sockaddr_storage* from, socklen_t* fromlen) {
  if (fd_ == -1) return Status::kIoError;

  // Same EINTR-safe bounded wait as TcpTransport::Read (libbrscan/
  // transport_tcp.cpp): SO_RCVTIMEO bounds one recvfrom() call, not a
  // retry loop, so track an absolute deadline and only give a retry after
  // a signal whatever time genuinely remains.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  uint8_t buf[2048];
  for (;;) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
            .count();
    if (remaining_ms <= 0) return Status::kTimeout;

    timeval tv{};
    tv.tv_sec = remaining_ms / 1000;
    tv.tv_usec = (remaining_ms % 1000) * 1000;
    if (setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
      return Status::kIoError;
    }

    sockaddr_storage sender{};
    socklen_t sender_len = sizeof(sender);
    const ssize_t n = recvfrom(fd_, buf, sizeof(buf), 0,
                                reinterpret_cast<sockaddr*>(&sender),
                                &sender_len);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return Status::kTimeout;
      return Status::kIoError;
    }

    *raw = std::vector<uint8_t>(buf, buf + n);
    *from = sender;
    *fromlen = sender_len;

    const std::optional<ButtonEvent> parsed =
        ParseNotification(buf, static_cast<size_t>(n));
    if (!parsed.has_value()) return Status::kProtocolError;
    *event = *parsed;
    return Status::kOk;
  }
}

Status ButtonListener::Ack(const std::vector<uint8_t>& raw,
                            const sockaddr_storage& to, socklen_t tolen) {
  if (fd_ == -1) return Status::kIoError;

  const ssize_t n = sendto(fd_, raw.data(), raw.size(), 0,
                            reinterpret_cast<const sockaddr*>(&to), tolen);
  if (n < 0 || static_cast<size_t>(n) != raw.size()) return Status::kIoError;
  return Status::kOk;
}

}  // namespace brscan::scand
