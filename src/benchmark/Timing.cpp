#include "csrun/benchmark/Timing.h"

namespace csrun {

std::string_view toString(TimingSource source) {
  switch (source) {
  case TimingSource::Unknown:
    return "unknown";
  case TimingSource::CpuWall:
    return "cpu_wall";
  case TimingSource::GpuTimestamp:
    return "gpu_timestamp";
  }
  return "unknown";
}

} // namespace csrun
