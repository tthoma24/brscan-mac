#include <gtest/gtest.h>
#include "fake_transport.h"

TEST(FakeTransport, ReplaysQueuedBytes) {
  brscan::FakeTransport t;
  t.QueueRead("+OK 200\r\n");

  uint8_t buf[32];
  size_t got = 0;
  ASSERT_EQ(t.Read(buf, sizeof(buf), &got, 1000), brscan::Status::kOk);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), got), "+OK 200\r\n");
}

TEST(FakeTransport, RecordsWrites) {
  brscan::FakeTransport t;
  const uint8_t cmd[] = {0x1b, 'Q', '\n', 0x80};
  ASSERT_EQ(t.Write(cmd, sizeof(cmd)), brscan::Status::kOk);
  EXPECT_EQ(t.written(), std::vector<uint8_t>(cmd, cmd + sizeof(cmd)));
}

TEST(FakeTransport, ReadReturnsTimeoutWhenEmpty) {
  brscan::FakeTransport t;
  uint8_t buf[8];
  size_t got = 0;
  EXPECT_EQ(t.Read(buf, sizeof(buf), &got, 0), brscan::Status::kTimeout);
}
