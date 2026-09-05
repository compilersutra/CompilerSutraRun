#include <gtest/gtest.h>

#include "csrun/validation/Comparator.h"

TEST(ComparatorTest, PassesWithinTolerance) {
  const float ref[] = {1.0f, 2.0f, 3.0f};
  const float act[] = {1.0f, 2.0000001f, 3.0f};
  csrun::ComparePolicy policy;
  auto r = csrun::Comparator::compareFloat32(ref, act, policy);
  EXPECT_TRUE(r.passed);
}

TEST(ComparatorTest, FailsOutsideTolerance) {
  const float ref[] = {1.0f, 2.0f};
  const float act[] = {1.0f, 9.0f};
  csrun::ComparePolicy policy;
  auto r = csrun::Comparator::compareFloat32(ref, act, policy);
  EXPECT_FALSE(r.passed);
  EXPECT_EQ(r.mismatchCount, 1u);
}
