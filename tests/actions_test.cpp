// Tests for the FUNC dispatch point (daemon/actions.h). Only FILE is
// implemented in this task; IMAGE/OCR/EMAIL are Task 5's job -- these
// tests just pin today's stand-in behavior (kOk, not an error) so a
// regression there is caught even before Task 5 lands.

#include "actions.h"

#include <gtest/gtest.h>

#include "config.h"

namespace brscan::scand {
namespace {

TEST(PerformActionTest, FileReturnsOk) {
  const Config cfg = DefaultConfig();
  EXPECT_EQ(PerformAction("FILE", "/tmp/whatever.jpg", cfg), Status::kOk);
}

TEST(PerformActionTest, UnimplementedFuncsReturnOkNotError) {
  const Config cfg = DefaultConfig();
  EXPECT_EQ(PerformAction("IMAGE", "/tmp/whatever.jpg", cfg), Status::kOk);
  EXPECT_EQ(PerformAction("OCR", "/tmp/whatever.pbm", cfg), Status::kOk);
  EXPECT_EQ(PerformAction("EMAIL", "/tmp/whatever.jpg", cfg), Status::kOk);
}

TEST(PerformActionTest, UnrecognizedFuncIsTreatedAsNoOp) {
  const Config cfg = DefaultConfig();
  EXPECT_EQ(PerformAction("BOGUS", "/tmp/whatever.jpg", cfg), Status::kOk);
}

}  // namespace
}  // namespace brscan::scand
