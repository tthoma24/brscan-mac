#include <cstdlib>

#include <gtest/gtest.h>
#include "fake_transport.h"
#include "session.h"
#include "transport_tcp.h"

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
