#include "snmp_register.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace brscan::scand {

namespace {

// BER/DER tags used by an SNMPv1 message. SET-REQUEST is PDU type 3 in the
// SNMP application class, encoded as a context-constructed tag (0xa0 | 3).
constexpr uint8_t kTagInteger = 0x02;
constexpr uint8_t kTagOctetString = 0x04;
constexpr uint8_t kTagSequence = 0x30;
constexpr uint8_t kTagSetRequest = 0xa3;

// Brother's button-destination-registration OID, 1.3.6.1.4.1.2435.2.3.9.2.
// 11.1.1.0, pre-encoded as a full OID TLV (tag 0x06, length 0x0f, then the
// 15-byte body). See reference/protocol-notes-button.md and the
// PROVENANCE.md row for this feature.
const std::vector<uint8_t>& ButtonOidTlv() {
  static const std::vector<uint8_t> oid = {
      0x06, 0x0f, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x93, 0x03,
      0x02, 0x03, 0x09, 0x02, 0x0b, 0x01, 0x01, 0x00};
  return oid;
}

// Appends a BER length octet (or octets) for `len` to `out`: short form
// (one byte) below 128, otherwise minimal-length long form -- a leading
// `0x80 | <number of length octets>` byte followed by the big-endian
// length. Every length a real registration produces fits in one or two
// long-form octets (the capture's outer SEQUENCE is 0x81 0x97), but the
// general long-form keeps the encoder correct rather than truncating a
// larger length while still claiming the shorter form: an earlier
// two-byte-max version silently emitted a corrupt `0x82 <hi> <lo>` for any
// length above 0xffff.
void AppendLength(std::vector<uint8_t>* out, size_t len) {
  if (len < 0x80) {
    out->push_back(static_cast<uint8_t>(len));
    return;
  }
  uint8_t octets[sizeof(size_t)];
  int count = 0;
  for (size_t v = len; v != 0; v >>= 8) {
    octets[count++] = static_cast<uint8_t>(v & 0xff);
  }
  out->push_back(static_cast<uint8_t>(0x80 | count));
  for (int i = count - 1; i >= 0; --i) out->push_back(octets[i]);
}

// Appends a full tag-length-value to `out`.
void AppendTlv(std::vector<uint8_t>* out, uint8_t tag,
               const std::vector<uint8_t>& content) {
  out->push_back(tag);
  AppendLength(out, content.size());
  out->insert(out->end(), content.begin(), content.end());
}

// Encodes a non-negative value as a BER INTEGER body: minimal-length,
// big-endian, two's-complement. Zero is a single 0x00 byte; a leading 0x00
// pad byte is prepended whenever the high bit of the first significant
// byte is set, so it isn't read back as negative (this is why the real
// capture's request-id 0xbc, whose high bit is set, encodes as two bytes:
// `02 02 00 bc`, not `02 01 bc`).
std::vector<uint8_t> EncodeUnsignedInteger(uint32_t value) {
  if (value == 0) return {0x00};

  std::vector<uint8_t> be;
  for (uint32_t v = value; v != 0; v >>= 8) {
    be.push_back(static_cast<uint8_t>(v & 0xff));
  }
  std::reverse(be.begin(), be.end());
  if (be.front() & 0x80) be.insert(be.begin(), 0x00);
  return be;
}

std::vector<uint8_t> ToBytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

}  // namespace

std::optional<std::string> SanitizeDisplayName(const std::string& name) {
  if (name.find('"') != std::string::npos ||
      name.find(';') != std::string::npos) {
    return std::nullopt;
  }
  return name;
}

std::string BuildRegisterValue(const std::string& ip, uint16_t port,
                                const std::string& name,
                                const std::string& func, int appnum,
                                int duration_sec) {
  std::ostringstream oss;
  oss << "TYPE=BR;BUTTON=SCAN;DURATION=" << duration_sec << ";CC=1;HOST="
      << ip << ":" << port << ";USER=\"" << name << "\";FUNC=" << func
      << ";APPNUM=" << appnum << ";";
  return oss.str();
}

std::vector<uint8_t> BuildSnmpSetRegister(const std::string& community,
                                           uint32_t request_id,
                                           const std::string& value) {
  // varbind = SEQUENCE { OID, OCTET STRING value }
  std::vector<uint8_t> varbind_content = ButtonOidTlv();
  AppendTlv(&varbind_content, kTagOctetString, ToBytes(value));
  std::vector<uint8_t> varbind;
  AppendTlv(&varbind, kTagSequence, varbind_content);

  // varbind-list = SEQUENCE OF varbind (exactly one, here).
  std::vector<uint8_t> varbind_list;
  AppendTlv(&varbind_list, kTagSequence, varbind);

  // SET-REQUEST PDU content: request-id, error-status=0, error-index=0,
  // varbind-list.
  std::vector<uint8_t> pdu_content;
  AppendTlv(&pdu_content, kTagInteger, EncodeUnsignedInteger(request_id));
  AppendTlv(&pdu_content, kTagInteger, EncodeUnsignedInteger(0));
  AppendTlv(&pdu_content, kTagInteger, EncodeUnsignedInteger(0));
  pdu_content.insert(pdu_content.end(), varbind_list.begin(),
                      varbind_list.end());
  std::vector<uint8_t> pdu;
  AppendTlv(&pdu, kTagSetRequest, pdu_content);

  // Message = SEQUENCE { version=0, community, pdu }.
  std::vector<uint8_t> message_content;
  AppendTlv(&message_content, kTagInteger, EncodeUnsignedInteger(0));
  AppendTlv(&message_content, kTagOctetString, ToBytes(community));
  message_content.insert(message_content.end(), pdu.begin(), pdu.end());
  std::vector<uint8_t> message;
  AppendTlv(&message, kTagSequence, message_content);
  return message;
}

std::vector<ResolvedEndpoint> DefaultSnmpResolver(const std::string& host) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  addrinfo* results = nullptr;
  std::vector<ResolvedEndpoint> endpoints;
  if (getaddrinfo(host.c_str(), "161", &hints, &results) != 0 ||
      results == nullptr) {
    return endpoints;
  }
  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    if (ai->ai_addr == nullptr ||
        ai->ai_addrlen > sizeof(sockaddr_storage)) {
      continue;
    }
    ResolvedEndpoint endpoint;
    std::memcpy(&endpoint.addr, ai->ai_addr, ai->ai_addrlen);
    endpoint.addr_len = ai->ai_addrlen;
    endpoints.push_back(endpoint);
  }
  freeaddrinfo(results);
  return endpoints;
}

bool DefaultSnmpSender(const ResolvedEndpoint& endpoint, const uint8_t* data,
                       std::size_t len) {
  const int fd = socket(endpoint.addr.ss_family, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return false;
  const ssize_t n =
      sendto(fd, data, len, 0,
             reinterpret_cast<const sockaddr*>(&endpoint.addr),
             endpoint.addr_len);
  close(fd);
  return n == static_cast<ssize_t>(len);
}

SnmpRegistrar::SnmpRegistrar(std::string printer_host, std::string community,
                             SnmpResolveFn resolver, SnmpSendFn sender)
    : printer_host_(std::move(printer_host)),
      community_(std::move(community)),
      resolver_(std::move(resolver)),
      sender_(std::move(sender)) {}

bool SnmpRegistrar::EnsureResolved() {
  if (resolved_) return true;
  ++resolve_count_;
  endpoints_ = resolver_ ? resolver_(printer_host_)
                         : DefaultSnmpResolver(printer_host_);
  resolved_ = !endpoints_.empty();
  return resolved_;
}

Status SnmpRegistrar::Send(const std::string& value, uint32_t request_id) {
  if (!EnsureResolved()) return Status::kIoError;

  const std::vector<uint8_t> packet =
      BuildSnmpSetRegister(community_, request_id, value);

  bool sent = false;
  for (const ResolvedEndpoint& endpoint : endpoints_) {
    const bool ok =
        sender_ ? sender_(endpoint, packet.data(), packet.size())
                : DefaultSnmpSender(endpoint, packet.data(), packet.size());
    if (ok) {
      sent = true;
      break;
    }
  }
  if (!sent) {
    // The cached address may be stale (printer moved) -- drop it so the next
    // Send() re-resolves rather than retrying a dead address forever.
    InvalidateCache();
    return Status::kIoError;
  }
  return Status::kOk;
}

void SnmpRegistrar::InvalidateCache() {
  endpoints_.clear();
  resolved_ = false;
}

Status RegisterDestinations(
    SnmpRegistrar& registrar, const std::vector<FuncRegistration>& funcs,
    const std::string& local_ip, uint16_t port, const std::string& name,
    int duration_sec, uint32_t* next_request_id,
    const std::function<void(const std::string& func, Status)>& on_result) {
  Status overall = Status::kOk;
  for (const FuncRegistration& f : funcs) {
    const std::string value = BuildRegisterValue(local_ip, port, name, f.func,
                                                 f.appnum, duration_sec);
    const Status status = registrar.Send(value, (*next_request_id)++);
    if (on_result) on_result(f.func, status);
    if (status != Status::kOk) overall = Status::kIoError;
  }
  return overall;
}

}  // namespace brscan::scand
