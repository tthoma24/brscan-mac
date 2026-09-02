#include <gtest/gtest.h>

#include "brscan/types.h"

TEST(Version, ReportsSemanticVersion) {
  EXPECT_STREQ(brscan::Version(), "0.1.0");
}
