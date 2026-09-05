// Tests for the paper-size table (daemon/paper_size.h). See PROVENANCE.md
// for the capture the 9 @300dpi area tuples below are transcribed from.

#include "paper_size.h"

#include <string>

#include <gtest/gtest.h>

#include "brscan/types.h"

namespace brscan::scand {
namespace {

void ExpectArea(const std::optional<brscan::Area>& got, int x0, int y0,
                 int x1, int y1) {
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->x0, x0);
  EXPECT_EQ(got->y0, y0);
  EXPECT_EQ(got->x1, x1);
  EXPECT_EQ(got->y1, y1);
}

// ---------------------------------------------------------------------
// All 9 captured tokens, exact @300 dpi.
// ---------------------------------------------------------------------

TEST(AreaForPaperTest, Letter300Exact) {
  ExpectArea(AreaForPaper("LETTER", 300), 478, 0, 2990, 3253);
}

TEST(AreaForPaperTest, Legal300Exact) {
  ExpectArea(AreaForPaper("LEGAL", 300), 478, 0, 2990, 4153);
}

TEST(AreaForPaperTest, A4_300Exact) {
  ExpectArea(AreaForPaper("A4", 300), 513, 0, 2961, 3461);
}

TEST(AreaForPaperTest, Ledger300Exact) {
  ExpectArea(AreaForPaper("LEDGER", 300), 103, 0, 3367, 5053);
}

TEST(AreaForPaperTest, A3_300Exact) {
  ExpectArea(AreaForPaper("A3", 300), 0, 0, 3472, 4913);
}

TEST(AreaForPaperTest, A5_300Exact) {
  ExpectArea(AreaForPaper("A5", 300), 0, 0, 1712, 2433);
}

TEST(AreaForPaperTest, Executive300Exact) {
  ExpectArea(AreaForPaper("EXECUTIVE", 300), 0, 0, 2128, 3103);
}

TEST(AreaForPaperTest, Photo300Exact) {
  ExpectArea(AreaForPaper("PHOTO", 300), 0, 0, 1168, 1753);
}

TEST(AreaForPaperTest, BCard300Exact) {
  ExpectArea(AreaForPaper("BCARD", 300), 0, 0, 1024, 661);
}

// ---------------------------------------------------------------------
// Scaling to other dpi.
// ---------------------------------------------------------------------

TEST(AreaForPaperTest, Letter200ScalesLinearlyWithToleranceOnX1) {
  // Brother's own 200dpi capture for LETTER is {319,0,1999,2169}: x0 and
  // y1 match this project's dpi/300.0-scaled-and-rounded formula exactly,
  // but x1 (2990 * 200/300 = 1993.33 -> 1993 here) is a few px off from
  // Brother's captured 1999. That's Brother's own per-dpi rounding, which
  // this project deliberately doesn't chase (see paper_size.cpp's design
  // comment and the Task 1d.2 brief) -- the device clips to the physical
  // page either way, so the drift is harmless. Assert the exact values
  // this project's formula produces for x0/y1, and only a tolerance
  // (+/-8) for x1.
  const auto area = AreaForPaper("LETTER", 200);
  ASSERT_TRUE(area.has_value());
  EXPECT_EQ(area->x0, 319);
  EXPECT_EQ(area->y0, 0);
  EXPECT_NEAR(area->x1, 1999, 8);
  EXPECT_EQ(area->y1, 2169);
}

TEST(AreaForPaperTest, Letter600IsThe300TupleDoubled) {
  ExpectArea(AreaForPaper("LETTER", 600), 956, 0, 5980, 6506);
}

// ---------------------------------------------------------------------
// Unknown token / invalid dpi.
// ---------------------------------------------------------------------

TEST(AreaForPaperTest, UnknownTokenReturnsNullopt) {
  // B4 is a real Brother paper size elsewhere in the protocol, but it is
  // not one of the 9 tokens the Scan-button LCD panel offers, so it's
  // "unknown" from this table's point of view.
  EXPECT_EQ(AreaForPaper("B4", 300), std::nullopt);
}

TEST(AreaForPaperTest, EmptyTokenReturnsNullopt) {
  EXPECT_EQ(AreaForPaper("", 300), std::nullopt);
}

TEST(AreaForPaperTest, NonPositiveDpiReturnsNullopt) {
  EXPECT_EQ(AreaForPaper("LETTER", 0), std::nullopt);
  EXPECT_EQ(AreaForPaper("LETTER", -300), std::nullopt);
}

TEST(IsKnownPaperTest, AcceptsAllNineTokens) {
  EXPECT_TRUE(IsKnownPaper("LETTER"));
  EXPECT_TRUE(IsKnownPaper("LEGAL"));
  EXPECT_TRUE(IsKnownPaper("A4"));
  EXPECT_TRUE(IsKnownPaper("LEDGER"));
  EXPECT_TRUE(IsKnownPaper("A3"));
  EXPECT_TRUE(IsKnownPaper("A5"));
  EXPECT_TRUE(IsKnownPaper("EXECUTIVE"));
  EXPECT_TRUE(IsKnownPaper("PHOTO"));
  EXPECT_TRUE(IsKnownPaper("BCARD"));
}

TEST(IsKnownPaperTest, RejectsUnknownAndCaseMismatch) {
  EXPECT_FALSE(IsKnownPaper("B4"));
  EXPECT_FALSE(IsKnownPaper(""));
  EXPECT_FALSE(IsKnownPaper("letter"));  // Case-sensitive per the wire protocol.
  EXPECT_FALSE(IsKnownPaper("BOGUS"));
}

}  // namespace
}  // namespace brscan::scand
