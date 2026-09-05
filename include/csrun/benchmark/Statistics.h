//===----------------------------------------------------------------------===//
// Statistics.h - Benchmark summary statistics
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BENCHMARK_STATISTICS_H
#define CSRUN_BENCHMARK_STATISTICS_H

#include "csrun/result/RunResult.h"

#include <span>
#include <vector>

namespace csrun {

class Statistics {
public:
  static TimingStats compute(std::span<const double> samplesMs, TimingSource source);
};

} // namespace csrun

#endif
