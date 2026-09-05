#include <gtest/gtest.h>

#include "csrun/benchmark/Statistics.h"

TEST(StatisticsTest, ComputesMedianAndMean) {
  const double samples[] = {1.0, 2.0, 3.0, 4.0};
  auto stats = csrun::Statistics::compute(samples, csrun::TimingSource::GpuTimestamp);
  EXPECT_EQ(stats.source, csrun::TimingSource::GpuTimestamp);
  EXPECT_DOUBLE_EQ(stats.minMs, 1.0);
  EXPECT_DOUBLE_EQ(stats.maxMs, 4.0);
  EXPECT_DOUBLE_EQ(stats.meanMs, 2.5);
  EXPECT_DOUBLE_EQ(stats.medianMs, 2.5);
}
