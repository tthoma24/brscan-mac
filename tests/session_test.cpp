#include <cstdlib>

#include <gtest/gtest.h>
#include "brscan/session.h"
#include "brscan/transport_tcp.h"
#include "fake_transport.h"

TEST(Session, AcceptsReadyGreeting) {
  brscan::FakeTransport t;
  t.QueueRead("+OK 200\r\n");
  brscan::Session s(&t);
  EXPECT_EQ(s.Open(), brscan::Status::kOk);
}

TEST(Session, ReportsBusyGreeting) {
  brscan::FakeTransport t;
  t.QueueRead("-NG 401\r\n");
  brscan::Session s(&t);
  EXPECT_EQ(s.Open(), brscan::Status::kBusy);
}

TEST(Session, RejectsUnknownGreeting) {
  brscan::FakeTransport t;
  t.QueueRead("garbage\r\n");
  brscan::Session s(&t);
  EXPECT_EQ(s.Open(), brscan::Status::kProtocolError);
}

// TCP may split the greeting across segments, so the first Read() can return
// fewer than the 7 bytes the "+OK 200" prefix needs. Open() must accumulate
// across reads rather than false-failing a healthy device on a short first
// segment. Here the greeting arrives in two chunks, the first only 4 bytes.
TEST(Session, AccumulatesFragmentedReadyGreeting) {
  brscan::FakeTransport t;
  t.QueueRead("+OK ");     // Short first segment (< 7 bytes).
  t.QueueRead("200\r\n");  // Remainder, completing the line.
  brscan::Session s(&t);
  EXPECT_EQ(s.Open(), brscan::Status::kOk);
}

// The same fragmentation for the busy greeting, split mid-prefix.
TEST(Session, AccumulatesFragmentedBusyGreeting) {
  brscan::FakeTransport t;
  t.QueueRead("-NG");
  t.QueueRead(" 401\r\n");
  brscan::Session s(&t);
  EXPECT_EQ(s.Open(), brscan::Status::kBusy);
}

// A device that goes quiet after a short partial greeting (no '\n', fewer
// than 7 bytes) times out rather than being misread as a valid greeting.
TEST(Session, TimesOutOnPartialGreetingThenSilence) {
  brscan::FakeTransport t;
  t.QueueRead("+OK");     // Partial prefix, then no more data.
  t.QueueTimeout();       // Stream goes quiet.
  brscan::Session s(&t);
  EXPECT_EQ(s.Open(), brscan::Status::kTimeout);
}

// Bounded-connect regression test. 192.0.2.1 is RFC 5737 TEST-NET-1,
// reserved for documentation: it is a routable-looking address that no host
// answers, so packets to it are silently dropped rather than promptly
// refused, which reliably reproduces the "unreachable device" case a
// blocking connect() with no timeout would hang on. A short
// connect_timeout_ms override keeps this fast instead of waiting out the
// production default (TcpTransport::kDefaultConnectTimeoutMs).
TEST(TcpTransport, ConnectTimesOutOnUnreachableHost) {
  brscan::TcpTransport t("192.0.2.1", 54921, /*connect_timeout_ms=*/300);
  EXPECT_EQ(t.Connect(), brscan::Status::kTimeout);
}

TEST(TcpTransportLive, GreetsWithOk) {
  const char* host = std::getenv("BRSCAN_TEST_HOST");
  if (host == nullptr) GTEST_SKIP() << "set BRSCAN_TEST_HOST to run";

  brscan::TcpTransport t(host, 54921);
  ASSERT_EQ(t.Connect(), brscan::Status::kOk);
  brscan::Session s(&t);
  EXPECT_EQ(s.Open(), brscan::Status::kOk);
  s.Close();
}
