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

TEST(FakeTransport, QueueTimeoutReturnsTimeoutThenContinues) {
  brscan::FakeTransport t;
  t.QueueRead("a");
  t.QueueTimeout();
  t.QueueRead("b");

  uint8_t buf[8];
  size_t got = 0;
  ASSERT_EQ(t.Read(buf, sizeof(buf), &got, 0), brscan::Status::kOk);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), got), "a");

  EXPECT_EQ(t.Read(buf, sizeof(buf), &got, 0), brscan::Status::kTimeout);

  ASSERT_EQ(t.Read(buf, sizeof(buf), &got, 0), brscan::Status::kOk);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), got), "b");
}

// A queued chunk bigger than the caller's buffer must be doled out over
// several Read() calls, like a real recv() on a TCP stream -- never
// silently dropped. This regression-tests a bug found in Task 7: an
// oversized queued chunk that fit in exactly one Read() call had its
// unread tail discarded when the whole entry was popped regardless of how
// much actually got copied out.
TEST(FakeTransport, OversizedChunkIsDoledOutAcrossReads) {
  brscan::FakeTransport t;
  std::vector<uint8_t> chunk(10, 0);
  for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<uint8_t>(i);
  t.QueueRead(chunk);

  std::vector<uint8_t> assembled;
  uint8_t buf[3];
  size_t got = 0;
  while (assembled.size() < chunk.size()) {
    ASSERT_EQ(t.Read(buf, sizeof(buf), &got, 0), brscan::Status::kOk);
    ASSERT_GT(got, 0u);
    assembled.insert(assembled.end(), buf, buf + got);
  }
  EXPECT_EQ(assembled, chunk);
  // Nothing left to read: the queue is now empty, not still holding a
  // stale (or corrupted) remainder.
  EXPECT_EQ(t.Read(buf, sizeof(buf), &got, 0), brscan::Status::kTimeout);
}
