#include <gtest/gtest.h>

#include "csrun/validation/Comparator.h"

#include <cmath>
#include <limits>
#include <vector>

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

TEST(ComparatorTest, KnownBadKernelOutputFails) {
  // Deliberately wrong device output must not pass validation.
  const float ref[] = {0.f, 1.f, 2.f, 3.f};
  const float act[] = {0.f, 1.f, 2.f, 999.f};
  csrun::ComparePolicy policy;
  policy.absoluteTolerance = 1e-5;
  policy.relativeTolerance = 1e-5;
  auto r = csrun::Comparator::compareFloat32(ref, act, policy);
  EXPECT_FALSE(r.passed);
  EXPECT_GE(r.mismatchCount, 1u);
  EXPECT_NE(r.message.find("FAIL"), std::string::npos);
}

TEST(ComparatorTest, SizeMismatchFails) {
  const float ref[] = {1.f, 2.f};
  const float act[] = {1.f};
  auto r = csrun::Comparator::compareFloat32(ref, act, {});
  EXPECT_FALSE(r.passed);
  EXPECT_EQ(r.message, "size mismatch");
}

TEST(ComparatorTest, AbsoluteOnlySmallDiffPassesCombinedPredicate) {
  // Predicate is abs AND rel; a tiny absolute error on a large magnitude can
  // exceed abs but stay under relative — must pass.
  const float ref[] = {1000.f};
  const float act[] = {1000.f + 1e-4f}; // abs=1e-4 > 1e-5, rel≈1e-7 < 1e-5
  csrun::ComparePolicy policy;
  policy.absoluteTolerance = 1e-5;
  policy.relativeTolerance = 1e-5;
  auto r = csrun::Comparator::compareFloat32(ref, act, policy);
  EXPECT_TRUE(r.passed);
}

TEST(ComparatorTest, BothAbsAndRelExceededFails) {
  const float ref[] = {1.f};
  const float act[] = {1.1f}; // abs=0.1, rel=0.1
  csrun::ComparePolicy policy;
  policy.absoluteTolerance = 1e-5;
  policy.relativeTolerance = 1e-5;
  auto r = csrun::Comparator::compareFloat32(ref, act, policy);
  EXPECT_FALSE(r.passed);
}

TEST(ComparatorTest, NanMismatchFails) {
  const float ref[] = {1.f};
  const float act[] = {std::numeric_limits<float>::quiet_NaN()};
  auto r = csrun::Comparator::compareFloat32(ref, act, {});
  EXPECT_FALSE(r.passed);
}

TEST(ComparatorTest, EmptySpansPass) {
  auto r = csrun::Comparator::compareFloat32({}, {}, {});
  EXPECT_TRUE(r.passed);
}
