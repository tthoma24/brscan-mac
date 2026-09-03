#include "snmp_register.h"

#include <algorithm>
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
// (one byte) below 128, long form (0x81 <len>, then 0x82 <hi> <lo>) at or
// above it. Every length this encoder produces (community/value strings,
// nested SEQUENCEs) comfortably fits in two-byte long form, so a
// three-byte form is not implemented.
void AppendLength(std::vector<uint8_t>* out, size_t len) {
  if (len < 0x80) {
    out->push_back(static_cast<uint8_t>(len));
  } else if (len <= 0xff) {
    out->push_back(0x81);
    out->push_back(static_cast<uint8_t>(len));
  } else {
    out->push_back(0x82);
    out->push_back(static_cast<uint8_t>((len >> 8) & 0xff));
    out->push_back(static_cast<uint8_t>(len & 0xff));
  }
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

Status SendSnmpRegister(const std::string& printer_host,
                         const std::string& community,
                         const std::string& value, uint32_t request_id) {
  const std::vector<uint8_t> packet =
      BuildSnmpSetRegister(community, request_id, value);

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  addrinfo* results = nullptr;
  const int rc =
      getaddrinfo(printer_host.c_str(), "161", &hints, &results);
  if (rc != 0 || results == nullptr) return Status::kIoError;

  Status status = Status::kIoError;
  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;

    const ssize_t n = sendto(fd, packet.data(), packet.size(), 0,
                              ai->ai_addr, ai->ai_addrlen);
    close(fd);
    if (n == static_cast<ssize_t>(packet.size())) {
      status = Status::kOk;
      break;
    }
  }

  freeaddrinfo(results);
  return status;
}

}  // namespace brscan::scand
