// Tests for the Scan-button UDP notification parser and listener
// (daemon/button_listener.h).
//
// PRIVACY: the real captured notification/ACK
// (reference/streams/button_notify_hex.txt, git-ignored) carries the
// developer's real LAN IP and computer name. Every payload below is
// synthetic (an RFC 5737 documentation IP, a placeholder name) and was
// built by hand to match the wire format studied from that capture -- see
// PROVENANCE.md and reference/protocol-notes-button.md. No bytes from the
// real capture appear below.

#include "button_listener.h"

#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace brscan::scand {
namespace {

// Builds a well-formed notification datagram for `payload` (the ASCII part
// after the 4-byte header), computing the header's length byte the way the
// real capture does: payload.size() + 5 (4 header bytes + 1; see the
// ParseNotification comment in button_listener.h). Payloads used here are
// all well under 250 bytes, so the length byte never overflows uint8_t.
std::vector<uint8_t> BuildDatagram(const std::string& payload) {
  std::vector<uint8_t> datagram;
  datagram.push_back(0x02);
  datagram.push_back(0x00);
  datagram.push_back(static_cast<uint8_t>(payload.size() + 5));
  datagram.push_back(0x30);
  datagram.insert(datagram.end(), payload.begin(), payload.end());
  return datagram;
}

std::string WellFormedPayload() {
  return "TYPE=BR;BUTTON=SCAN;USER=\"Test Mac\";FUNC=FILE;"
         "HOST=192.0.2.10:54925;APPNUM=5;P1=0;P2=0;P3=0;P4=0;"
         "REGID=12345;SEQ=3;";
}

TEST(ParseNotificationTest, ParsesWellFormedNotification) {
  const std::vector<uint8_t> datagram = BuildDatagram(WellFormedPayload());

  const std::optional<ButtonEvent> event =
      ParseNotification(datagram.data(), datagram.size());

  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->func, "FILE");
  EXPECT_EQ(event->user, "Test Mac");
  EXPECT_EQ(event->host_ip, "192.0.2.10");
  EXPECT_EQ(event->host_port, 54925);
  EXPECT_EQ(event->appnum, 5);
  EXPECT_EQ(event->regid, "12345");
  EXPECT_EQ(event->seq, 3);
}

TEST(ParseNotificationTest, ParsesEachFunc) {
  struct Case {
    std::string func;
    int appnum;
  };
  const Case cases[] = {
      {"FILE", 5}, {"IMAGE", 1}, {"EMAIL", 2}, {"OCR", 3}};
  for (const Case& c : cases) {
    const std::string payload =
        "TYPE=BR;BUTTON=SCAN;USER=\"Test Mac\";FUNC=" + c.func +
        ";HOST=192.0.2.10:54925;APPNUM=" + std::to_string(c.appnum) +
        ";P1=0;P2=0;P3=0;P4=0;REGID=1;SEQ=1;";
    const std::vector<uint8_t> datagram = BuildDatagram(payload);

    const std::optional<ButtonEvent> event =
        ParseNotification(datagram.data(), datagram.size());

    ASSERT_TRUE(event.has_value()) << "FUNC=" << c.func;
    EXPECT_EQ(event->func, c.func);
    EXPECT_EQ(event->appnum, c.appnum);
  }
}

TEST(ParseNotificationTest, RejectsNullData) {
  EXPECT_FALSE(ParseNotification(nullptr, 0).has_value());
}

TEST(ParseNotificationTest, RejectsTooShort) {
  const std::vector<uint8_t> datagram = {0x02, 0x00, 0x05};  // 3 bytes < header.
  EXPECT_FALSE(ParseNotification(datagram.data(), datagram.size()).has_value());
}

TEST(ParseNotificationTest, RejectsEmptyBuffer) {
  EXPECT_FALSE(ParseNotification(nullptr, 0).has_value());
}

TEST(ParseNotificationTest, RejectsMissingTypeBr) {
  const std::string payload =
      "TYPE=XX;BUTTON=SCAN;USER=\"Test Mac\";FUNC=FILE;"
      "HOST=192.0.2.10:54925;APPNUM=5;REGID=1;SEQ=1;";
  const std::vector<uint8_t> datagram = BuildDatagram(payload);
  EXPECT_FALSE(ParseNotification(datagram.data(), datagram.size()).has_value());
}

TEST(ParseNotificationTest, RejectsMissingButtonScan) {
  const std::string payload =
      "TYPE=BR;BUTTON=OTHER;USER=\"Test Mac\";FUNC=FILE;"
      "HOST=192.0.2.10:54925;APPNUM=5;REGID=1;SEQ=1;";
  const std::vector<uint8_t> datagram = BuildDatagram(payload);
  EXPECT_FALSE(ParseNotification(datagram.data(), datagram.size()).has_value());
}

TEST(ParseNotificationTest, RejectsTruncatedMidKey) {
  // Cut off partway through "FUNC=FILE": the trailing "FUN" token has no
  // '=' at all once split on ';'. The header's length byte matches this
  // shorter payload's actual size, so the failure comes from the bad
  // token / missing required fields, not from the length check.
  const std::string truncated_payload =
      "TYPE=BR;BUTTON=SCAN;USER=\"Test Mac\";FUN";
  const std::vector<uint8_t> datagram = BuildDatagram(truncated_payload);

  EXPECT_FALSE(ParseNotification(datagram.data(), datagram.size()).has_value());
}

TEST(ParseNotificationTest, RejectsLengthByteThatUnderstatesPayloadSize) {
  const std::vector<uint8_t> datagram = BuildDatagram(WellFormedPayload());
  std::vector<uint8_t> corrupted = datagram;
  // Otherwise-well-formed and complete; only the header's length byte is
  // wrong (understates the real payload size that follows it).
  corrupted[2] = static_cast<uint8_t>(datagram[2] - 10);

  EXPECT_FALSE(ParseNotification(corrupted.data(), corrupted.size()).has_value());
}

TEST(ParseNotificationTest, RejectsLengthByteThatOverstatesPayloadSize) {
  const std::vector<uint8_t> datagram = BuildDatagram(WellFormedPayload());
  std::vector<uint8_t> corrupted = datagram;
  // Otherwise-well-formed and complete; only the header's length byte is
  // wrong (claims more payload than actually arrived).
  corrupted[2] = static_cast<uint8_t>(datagram[2] + 10);

  EXPECT_FALSE(ParseNotification(corrupted.data(), corrupted.size()).has_value());
}

TEST(ParseNotificationTest, RejectsNonNumericAppnum) {
  const std::string payload =
      "TYPE=BR;BUTTON=SCAN;USER=\"Test Mac\";FUNC=FILE;"
      "HOST=192.0.2.10:54925;APPNUM=abc;REGID=1;SEQ=1;";
  const std::vector<uint8_t> datagram = BuildDatagram(payload);
  EXPECT_FALSE(ParseNotification(datagram.data(), datagram.size()).has_value());
}

TEST(ParseNotificationTest, RejectsNonNumericSeq) {
  const std::string payload =
      "TYPE=BR;BUTTON=SCAN;USER=\"Test Mac\";FUNC=FILE;"
      "HOST=192.0.2.10:54925;APPNUM=5;REGID=1;SEQ=xyz;";
  const std::vector<uint8_t> datagram = BuildDatagram(payload);
  EXPECT_FALSE(ParseNotification(datagram.data(), datagram.size()).has_value());
}

TEST(ParseNotificationTest, RejectsBadHeaderConstantByte) {
  std::vector<uint8_t> datagram = BuildDatagram(WellFormedPayload());
  datagram[3] = 0x31;  // Should always be 0x30.
  EXPECT_FALSE(ParseNotification(datagram.data(), datagram.size()).has_value());
}

TEST(ParseNotificationTest, RejectsMissingHostPort) {
  const std::string payload =
      "TYPE=BR;BUTTON=SCAN;USER=\"Test Mac\";FUNC=FILE;"
      "HOST=192.0.2.10;APPNUM=5;REGID=1;SEQ=1;";
  const std::vector<uint8_t> datagram = BuildDatagram(payload);
  EXPECT_FALSE(ParseNotification(datagram.data(), datagram.size()).has_value());
}

TEST(ParseNotificationTest, RejectsUnterminatedQuotedValue) {
  // USER's opening quote is never closed (e.g. a datagram truncated
  // mid-name): must fail closed rather than pass the leading '"' through
  // as part of the user name.
  const std::string payload =
      "TYPE=BR;BUTTON=SCAN;USER=\"Test Mac;FUNC=FILE;"
      "HOST=192.0.2.10:54925;APPNUM=5;REGID=1;SEQ=1;";
  const std::vector<uint8_t> datagram = BuildDatagram(payload);
  EXPECT_FALSE(ParseNotification(datagram.data(), datagram.size()).has_value());
}

// Loopback UDP test: bind a ButtonListener on an ephemeral port (0, since
// binding the real 54925 is not reliable in every test environment -- see
// button_listener.h's ButtonListener(uint16_t port) comment), send it a
// synthetic notification from a second UDP socket, Receive() + parse it,
// Ack() it, and confirm the ack arrives back at the sender byte-for-byte
// equal to what was sent.
TEST(ButtonListenerLoopbackTest, ReceivesParsesAndAcksOverLoopback) {
  ButtonListener listener(/*port=*/0);
  ASSERT_EQ(listener.Bind(), Status::kOk);

  const int sender_fd = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(sender_fd, 0);
  sockaddr_in sender_addr{};
  sender_addr.sin_family = AF_INET;
  sender_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sender_addr.sin_port = 0;
  ASSERT_EQ(bind(sender_fd, reinterpret_cast<sockaddr*>(&sender_addr),
                 sizeof(sender_addr)),
            0);

  sockaddr_in listener_addr{};
  listener_addr.sin_family = AF_INET;
  listener_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  listener_addr.sin_port = htons(listener.port());

  const std::vector<uint8_t> sent = BuildDatagram(WellFormedPayload());
  const ssize_t sent_n =
      sendto(sender_fd, sent.data(), sent.size(), 0,
             reinterpret_cast<sockaddr*>(&listener_addr), sizeof(listener_addr));
  ASSERT_EQ(sent_n, static_cast<ssize_t>(sent.size()));

  ButtonEvent event;
  std::vector<uint8_t> raw;
  sockaddr_storage from{};
  socklen_t fromlen = sizeof(from);
  const Status receive_status =
      listener.Receive(/*timeout_ms=*/2000, &event, &raw, &from, &fromlen);
  ASSERT_EQ(receive_status, Status::kOk);
  EXPECT_EQ(raw, sent);
  EXPECT_EQ(event.func, "FILE");
  EXPECT_EQ(event.seq, 3);

  ASSERT_EQ(listener.Ack(raw, from, fromlen), Status::kOk);

  std::vector<uint8_t> ack_buf(sent.size() + 16);
  const ssize_t ack_n =
      recv(sender_fd, ack_buf.data(), ack_buf.size(), 0);
  ASSERT_EQ(ack_n, static_cast<ssize_t>(sent.size()));
  ack_buf.resize(ack_n);
  EXPECT_EQ(ack_buf, sent);

  close(sender_fd);
}

TEST(ButtonListenerTest, SecondBindFails) {
  ButtonListener listener(/*port=*/0);
  ASSERT_EQ(listener.Bind(), Status::kOk);
  // A second Bind() must not silently replace fd_ (which would leak the
  // first socket); it should fail instead.
  EXPECT_EQ(listener.Bind(), Status::kIoError);
}

TEST(ButtonListenerTest, ReceiveTimesOutWithNoDatagram) {
  ButtonListener listener(/*port=*/0);
  ASSERT_EQ(listener.Bind(), Status::kOk);

  ButtonEvent event;
  std::vector<uint8_t> raw;
  sockaddr_storage from{};
  socklen_t fromlen = sizeof(from);
  EXPECT_EQ(listener.Receive(/*timeout_ms=*/100, &event, &raw, &from, &fromlen),
            Status::kTimeout);
}

}  // namespace
}  // namespace brscan::scand
