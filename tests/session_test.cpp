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

TEST(TcpTransportLive, GreetsWithOk) {
  const char* host = std::getenv("BRSCAN_TEST_HOST");
  if (host == nullptr) GTEST_SKIP() << "set BRSCAN_TEST_HOST to run";

  brscan::TcpTransport t(host, 54921);
  ASSERT_EQ(t.Connect(), brscan::Status::kOk);
  brscan::Session s(&t);
  EXPECT_EQ(s.Open(), brscan::Status::kOk);
  s.Close();
}
