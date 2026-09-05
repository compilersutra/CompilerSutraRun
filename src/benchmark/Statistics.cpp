#include "csrun/benchmark/Statistics.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace csrun {

TimingStats Statistics::compute(std::span<const double> samplesMs, TimingSource source) {
  TimingStats stats;
  stats.source = source;
  if (samplesMs.empty())
    return stats;

  std::vector<double> sorted(samplesMs.begin(), samplesMs.end());
  std::sort(sorted.begin(), sorted.end());

  stats.minMs = sorted.front();
  stats.maxMs = sorted.back();
  stats.meanMs = std::accumulate(sorted.begin(), sorted.end(), 0.0) / sorted.size();

  const std::size_t n = sorted.size();
  if (n % 2 == 1)
    stats.medianMs = sorted[n / 2];
  else
    stats.medianMs = 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);

  double accum = 0.0;
  for (double v : sorted) {
    const double d = v - stats.meanMs;
    accum += d * d;
  }
  stats.stddevMs = std::sqrt(accum / sorted.size());
  return stats;
}

} // namespace csrun
