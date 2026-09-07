// Tests for the SNMPv1 Set BER encoder used to register this Mac as a
// Scan-button destination (daemon/snmp_register.h).
//
// PRIVACY: the real captured registration packet
// (reference/streams/button_snmp_set.bin, git-ignored) carries the
// developer's real LAN IP and computer name. Every expected value and byte
// string here is synthetic (RFC 5737 documentation IP, a placeholder name)
// and was verified by hand against the captured packet's BER *layout* --
// see PROVENANCE.md and reference/protocol-notes-button.md. No bytes from
// the real capture appear below.

#include "snmp_register.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace brscan::scand {
namespace {

TEST(SanitizeDisplayNameTest, AcceptsAnOrdinaryName) {
  const auto sanitized = SanitizeDisplayName("Office Mac");
  ASSERT_TRUE(sanitized.has_value());
  EXPECT_EQ(*sanitized, "Office Mac");
}

TEST(SanitizeDisplayNameTest, RejectsEmbeddedQuote) {
  // A '"' would close USER="..." early, letting the rest of the name
  // inject its own KEY=VALUE tokens into the registration string.
  EXPECT_FALSE(SanitizeDisplayName("Evil\" ;FUNC=IMAGE;\"Mac").has_value());
  EXPECT_FALSE(SanitizeDisplayName("\"").has_value());
}

TEST(SanitizeDisplayNameTest, RejectsEmbeddedSemicolon) {
  // A ';' would terminate the USER field's token and start a new
  // KEY=VALUE pair of the attacker's/typo's choosing.
  EXPECT_FALSE(SanitizeDisplayName("Mac;FUNC=IMAGE").has_value());
  EXPECT_FALSE(SanitizeDisplayName(";").has_value());
}

TEST(BuildRegisterValueTest, ComposesExpectedString) {
  EXPECT_EQ(BuildRegisterValue("192.0.2.10", 54925, "Test Mac", "FILE", 5,
                                /*duration_sec=*/360),
            "TYPE=BR;BUTTON=SCAN;DURATION=360;CC=1;HOST=192.0.2.10:54925;"
            "USER=\"Test Mac\";FUNC=FILE;APPNUM=5;");
}

TEST(BuildRegisterValueTest, DefaultsDurationTo360) {
  EXPECT_EQ(BuildRegisterValue("1.2.3.4", 1, "A", "OCR", 3),
            "TYPE=BR;BUTTON=SCAN;DURATION=360;CC=1;HOST=1.2.3.4:1;"
            "USER=\"A\";FUNC=OCR;APPNUM=3;");
}

TEST(BuildRegisterValueTest, CarriesEachFuncAndAppNum) {
  EXPECT_EQ(BuildRegisterValue("198.51.100.5", 54925, "Office Mac", "IMAGE",
                                kAppNumImage, 60),
            "TYPE=BR;BUTTON=SCAN;DURATION=60;CC=1;HOST=198.51.100.5:54925;"
            "USER=\"Office Mac\";FUNC=IMAGE;APPNUM=1;");
  EXPECT_EQ(BuildRegisterValue("198.51.100.5", 54925, "Office Mac", "EMAIL",
                                kAppNumEmail, 60),
            "TYPE=BR;BUTTON=SCAN;DURATION=60;CC=1;HOST=198.51.100.5:54925;"
            "USER=\"Office Mac\";FUNC=EMAIL;APPNUM=2;");
}

// Expected bytes below were built by hand from the SNMPv1 BER structure in
// reference/protocol-notes-button.md:
//
//   SEQUENCE {
//     INTEGER version = 0
//     OCTET STRING community
//     [3] SET-REQUEST {
//       INTEGER request-id
//       INTEGER error-status = 0
//       INTEGER error-index = 0
//       SEQUENCE {              -- varbind list
//         SEQUENCE {            -- one varbind
//           OID 1.3.6.1.4.1.2435.2.3.9.2.11.1.1.0
//           OCTET STRING value
//         }
//       }
//     }
//   }
//
// and cross-checked field-by-field (tag/length framing, the 17-byte OID
// encoding, the community and request-id bytes) against the real capture's
// structure -- see the PROVENANCE.md row for this feature.

// This case's value is 95 bytes, making the outer SEQUENCE's content 144
// bytes and the SET-REQUEST's content exactly 128 bytes: both cross the
// 128-byte threshold where BER length switches from short form (one byte)
// to long form (0x81 <len>), the same long-form shape the real 154-byte
// capture uses (its outer SEQUENCE is 0x81 0x97).
TEST(BuildSnmpSetRegisterTest, EncodesLongFormLengths) {
  const std::string value =
      "TYPE=BR;BUTTON=SCAN;DURATION=360;CC=1;HOST=192.0.2.10:54925;"
      "USER=\"Test Mac\";FUNC=FILE;APPNUM=5;";
  ASSERT_EQ(value.size(), 95u);

  const std::vector<uint8_t> expected = {
      0x30, 0x81, 0x90,                                     // SEQUENCE, len 144 (long form)
      0x02, 0x01, 0x00,                                     // INTEGER version = 0
      0x04, 0x08, 'i', 'n', 't', 'e', 'r', 'n', 'a', 'l',    // OCTET STRING "internal"
      0xa3, 0x81, 0x80,                                      // [3] SET-REQUEST, len 128 (long form)
      0x02, 0x02, 0x00, 0xbc,                                // INTEGER request-id = 0x00bc
      0x02, 0x01, 0x00,                                      // INTEGER error-status = 0
      0x02, 0x01, 0x00,                                      // INTEGER error-index = 0
      0x30, 0x74,                                            // SEQUENCE varbind-list, len 116
      0x30, 0x72,                                            // SEQUENCE varbind, len 114
      0x06, 0x0f,                                            // OID, len 15
      0x2b, 0x06, 0x01, 0x04, 0x01, 0x93, 0x03, 0x02,
      0x03, 0x09, 0x02, 0x0b, 0x01, 0x01, 0x00,
      0x04, 0x5f,                                            // OCTET STRING value, len 95
      'T', 'Y', 'P', 'E', '=', 'B', 'R', ';', 'B', 'U', 'T',
      'T', 'O', 'N', '=', 'S', 'C', 'A', 'N', ';', 'D', 'U',
      'R', 'A', 'T', 'I', 'O', 'N', '=', '3', '6', '0', ';',
      'C', 'C', '=', '1', ';', 'H', 'O', 'S', 'T', '=', '1',
      '9', '2', '.', '0', '.', '2', '.', '1', '0', ':', '5',
      '4', '9', '2', '5', ';', 'U', 'S', 'E', 'R', '=', '"',
      'T', 'e', 's', 't', ' ', 'M', 'a', 'c', '"', ';', 'F',
      'U', 'N', 'C', '=', 'F', 'I', 'L', 'E', ';', 'A', 'P',
      'P', 'N', 'U', 'M', '=', '5', ';',
  };

  const std::vector<uint8_t> actual =
      BuildSnmpSetRegister("internal", 0x00bc, value);
  EXPECT_EQ(actual, expected);
}

// A short value keeps every length in the structure under 128, so every
// length octet in this packet uses short form -- including the outer
// SEQUENCE, which the long-form case above cannot exercise.
TEST(BuildSnmpSetRegisterTest, EncodesShortFormLengths) {
  const std::string value =
      "TYPE=BR;BUTTON=SCAN;DURATION=60;CC=1;HOST=1.2.3.4:1;"
      "USER=\"A\";FUNC=OCR;APPNUM=3;";
  ASSERT_EQ(value.size(), 79u);

  const std::vector<uint8_t> expected = {
      0x30, 0x7e,                                          // SEQUENCE, len 126 (short form)
      0x02, 0x01, 0x00,                                     // INTEGER version = 0
      0x04, 0x08, 'i', 'n', 't', 'e', 'r', 'n', 'a', 'l',    // OCTET STRING "internal"
      0xa3, 0x6f,                                            // [3] SET-REQUEST, len 111 (short form)
      0x02, 0x01, 0x07,                                      // INTEGER request-id = 7
      0x02, 0x01, 0x00,                                      // INTEGER error-status = 0
      0x02, 0x01, 0x00,                                      // INTEGER error-index = 0
      0x30, 0x64,                                            // SEQUENCE varbind-list, len 100
      0x30, 0x62,                                            // SEQUENCE varbind, len 98
      0x06, 0x0f,                                            // OID, len 15
      0x2b, 0x06, 0x01, 0x04, 0x01, 0x93, 0x03, 0x02,
      0x03, 0x09, 0x02, 0x0b, 0x01, 0x01, 0x00,
      0x04, 0x4f,                                            // OCTET STRING value, len 79
      'T', 'Y', 'P', 'E', '=', 'B', 'R', ';', 'B', 'U', 'T',
      'T', 'O', 'N', '=', 'S', 'C', 'A', 'N', ';', 'D', 'U',
      'R', 'A', 'T', 'I', 'O', 'N', '=', '6', '0', ';', 'C',
      'C', '=', '1', ';', 'H', 'O', 'S', 'T', '=', '1', '.',
      '2', '.', '3', '.', '4', ':', '1', ';', 'U', 'S', 'E',
      'R', '=', '"', 'A', '"', ';', 'F', 'U', 'N', 'C', '=',
      'O', 'C', 'R', ';', 'A', 'P', 'P', 'N', 'U', 'M', '=',
      '3', ';',
  };

  const std::vector<uint8_t> actual = BuildSnmpSetRegister("internal", 7, value);
  EXPECT_EQ(actual, expected);
}

// A value large enough to push the enclosing lengths past 0xffff exercises
// AppendLength's three-octet long form (0x83 <hi> <mid> <lo>), which the
// capture-sized packets never reach. Regression guard: a two-byte-max
// encoder silently truncated any length above 0xffff to two octets while
// still emitting the 0x82 marker, corrupting the packet.
TEST(BuildSnmpSetRegisterTest, EncodesThreeOctetLongFormForLargeValues) {
  const std::string big_value(70000, 'X');
  const std::vector<uint8_t> packet =
      BuildSnmpSetRegister("internal", 1, big_value);

  // Outer SEQUENCE: tag 0x30, then a long-form length. ~70 KB of content
  // needs three length octets, so the length-of-length byte is 0x83 and the
  // three that follow are the big-endian content length.
  ASSERT_GE(packet.size(), 5u);
  EXPECT_EQ(packet[0], 0x30);
  EXPECT_EQ(packet[1], 0x83);
  const size_t declared = (static_cast<size_t>(packet[2]) << 16) |
                          (static_cast<size_t>(packet[3]) << 8) |
                          static_cast<size_t>(packet[4]);
  // The declared length must match the actual bytes after the 5-byte
  // tag+length header -- i.e. the length wasn't truncated.
  EXPECT_EQ(declared, packet.size() - 5);
}

TEST(BuildSnmpSetRegisterTest, RequestIdVariesOtherFieldsDoNot) {
  const std::string value = "TYPE=BR;BUTTON=SCAN;DURATION=60;CC=1;"
                             "HOST=1.2.3.4:1;USER=\"A\";FUNC=OCR;APPNUM=3;";
  const std::vector<uint8_t> a = BuildSnmpSetRegister("internal", 1, value);
  const std::vector<uint8_t> b = BuildSnmpSetRegister("internal", 2, value);
  ASSERT_EQ(a.size(), b.size());
  // Only the request-id INTEGER's value byte (index 19: 0x30 0x7e 0x02 0x01
  // 0x00 0x04 0x08 "internal"(8) 0xa3 0x6f 0x02 0x01 <id>) should differ.
  size_t differences = 0;
  size_t first_diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) {
      ++differences;
      first_diff = i;
    }
  }
  EXPECT_EQ(differences, 1u);
  EXPECT_EQ(first_diff, 19u);
}

// --- SnmpRegistrar address caching (GitHub #22) ---
//
// These use the injectable resolver/sender seams so no real name resolution
// or UDP send happens. A single fake endpoint (all-zero address) is enough:
// the fake sender never inspects it.

namespace {

// A resolver that hands back one dummy endpoint and counts how many times it
// was asked to resolve.
SnmpResolveFn CountingResolver(int* calls) {
  return [calls](const std::string&) {
    ++*calls;
    return std::vector<ResolvedEndpoint>{ResolvedEndpoint{}};
  };
}

// Pulls the FUNC=... token out of an encoded registration packet, so a fake
// sender can capture which FUNC each Set carried.
std::string FuncInPacket(const uint8_t* data, std::size_t len) {
  const std::string s(data, data + len);
  const std::string key = "FUNC=";
  const auto pos = s.find(key);
  if (pos == std::string::npos) return "";
  const auto start = pos + key.size();
  const auto end = s.find(';', start);
  return s.substr(start, end == std::string::npos ? end : end - start);
}

}  // namespace

TEST(SnmpRegistrarTest, ResolvesOnceAndReusesAcrossSends) {
  int resolves = 0;
  auto sender = [](const ResolvedEndpoint&, const uint8_t*, std::size_t) {
    return true;
  };
  SnmpRegistrar registrar("printer.local", "internal",
                          CountingResolver(&resolves), sender);

  for (uint32_t i = 1; i <= 4; ++i) {
    EXPECT_EQ(registrar.Send("value", i), Status::kOk);
  }
  // Four successful Sets, but the host was resolved exactly once.
  EXPECT_EQ(resolves, 1);
  EXPECT_EQ(registrar.resolve_count(), 1);
}

TEST(SnmpRegistrarTest, ReResolvesAfterAFailedSend) {
  int resolves = 0;
  bool fail_send = false;
  auto sender = [&fail_send](const ResolvedEndpoint&, const uint8_t*,
                             std::size_t) { return !fail_send; };
  SnmpRegistrar registrar("printer.local", "internal",
                          CountingResolver(&resolves), sender);

  EXPECT_EQ(registrar.Send("value", 1), Status::kOk);  // Resolves (count 1).
  EXPECT_EQ(registrar.resolve_count(), 1);

  fail_send = true;
  EXPECT_EQ(registrar.Send("value", 2), Status::kIoError);  // Drops the cache.

  fail_send = false;
  EXPECT_EQ(registrar.Send("value", 3), Status::kOk);  // Re-resolves (count 2).
  EXPECT_EQ(registrar.resolve_count(), 2);
}

TEST(SnmpRegistrarTest, RetriesResolveWhenHostIsUnresolvable) {
  int resolves = 0;
  auto empty_resolver = [&resolves](const std::string&) {
    ++resolves;
    return std::vector<ResolvedEndpoint>{};  // Resolution failed.
  };
  auto sender = [](const ResolvedEndpoint&, const uint8_t*, std::size_t) {
    return true;
  };
  SnmpRegistrar registrar("nope.local", "internal", empty_resolver, sender);

  // Each Send re-attempts resolution while it keeps failing (nothing to
  // cache), rather than giving up after the first miss.
  EXPECT_EQ(registrar.Send("value", 1), Status::kIoError);
  EXPECT_EQ(registrar.Send("value", 2), Status::kIoError);
  EXPECT_EQ(resolves, 2);
}

TEST(SnmpRegistrarTest, InvalidateCacheForcesReResolve) {
  int resolves = 0;
  auto sender = [](const ResolvedEndpoint&, const uint8_t*, std::size_t) {
    return true;
  };
  SnmpRegistrar registrar("printer.local", "internal",
                          CountingResolver(&resolves), sender);

  EXPECT_EQ(registrar.Send("value", 1), Status::kOk);
  registrar.InvalidateCache();
  EXPECT_EQ(registrar.Send("value", 2), Status::kOk);
  EXPECT_EQ(resolves, 2);
}

TEST(RegisterDestinationsTest, SendsOnlyTheGivenFuncsInOrder) {
  int resolves = 0;
  std::vector<std::string> sent_funcs;
  auto sender = [&sent_funcs](const ResolvedEndpoint&, const uint8_t* data,
                              std::size_t len) {
    sent_funcs.push_back(FuncInPacket(data, len));
    return true;
  };
  SnmpRegistrar registrar("printer.local", "internal",
                          CountingResolver(&resolves), sender);

  // A configured subset: FILE + EMAIL only.
  const std::vector<FuncRegistration> funcs = {
      {"FILE", kAppNumFile}, {"EMAIL", kAppNumEmail}};
  uint32_t request_id = 1;
  const Status status =
      RegisterDestinations(registrar, funcs, "192.0.2.10", 54925, "Office Mac",
                           /*duration_sec=*/360, &request_id);

  EXPECT_EQ(status, Status::kOk);
  EXPECT_EQ(sent_funcs, (std::vector<std::string>{"FILE", "EMAIL"}));
  EXPECT_EQ(request_id, 3u);  // Two Sets consumed request ids 1 and 2.
  EXPECT_EQ(registrar.resolve_count(), 1);  // Both Sets shared one resolution.
}

TEST(RegisterDestinationsTest, ReportsFailureWhenAFuncFailsToSend) {
  auto resolver = [](const std::string&) {
    return std::vector<ResolvedEndpoint>{ResolvedEndpoint{}};
  };
  auto sender = [](const ResolvedEndpoint&, const uint8_t*, std::size_t) {
    return false;  // Every Set fails to send.
  };
  SnmpRegistrar registrar("printer.local", "internal", resolver, sender);

  std::vector<std::string> results;
  const std::vector<FuncRegistration> funcs = {{"FILE", kAppNumFile}};
  uint32_t request_id = 1;
  const Status status = RegisterDestinations(
      registrar, funcs, "192.0.2.10", 54925, "Mac", 360, &request_id,
      [&results](const std::string& func, Status s) {
        results.push_back(func + (s == Status::kOk ? ":ok" : ":fail"));
      });

  EXPECT_EQ(status, Status::kIoError);
  EXPECT_EQ(results, (std::vector<std::string>{"FILE:fail"}));
}

}  // namespace
}  // namespace brscan::scand
