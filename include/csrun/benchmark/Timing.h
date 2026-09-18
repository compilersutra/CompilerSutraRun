//===----------------------------------------------------------------------===//
//
// Timing.h - Shared timing enums/stats (benchmark ↔ result)
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BENCHMARK_TIMING_H
#define CSRUN_BENCHMARK_TIMING_H

#include <string_view>

namespace csrun {

enum class TimingSource { Unknown, CpuWall, GpuTimestamp };

struct TimingStats {
  TimingSource source = TimingSource::Unknown;
  double minMs = 0;
  double medianMs = 0;
  double meanMs = 0;
  double maxMs = 0;
  double stddevMs = 0;
};

std::string_view toString(TimingSource source);

} // namespace csrun

#endif
