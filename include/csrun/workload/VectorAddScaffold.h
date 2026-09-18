//===----------------------------------------------------------------------===//
//
// VectorAddScaffold.h - Shared host prep / kernel path / result fill
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_WORKLOAD_VECTORADDSCAFFOLD_H
#define CSRUN_WORKLOAD_VECTORADDSCAFFOLD_H

#include "csrun/backend/BackendType.h"
#include "csrun/backend/ExecuteOptions.h"
#include "csrun/benchmark/Timing.h"
#include "csrun/core/Expected.h"
#include "csrun/device/DeviceInfo.h"
#include "csrun/result/RunResult.h"
#include "csrun/workload/Workload.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace csrun {

struct VectorAddHost {
  std::vector<float> a;
  std::vector<float> b;
  std::vector<float> reference;
  std::vector<float> output;
  std::uint32_t count = 0;
};

/// Reject non-vector_add / non-fp32 / empty count with a consistent Error.
Expected<std::uint32_t> requireVectorAddFp32(const Workload &workload,
                                            std::string_view backendName);

VectorAddHost makeVectorAddHost(std::uint32_t count);

/// Resolve kernel next to YAML, then cwd fallbacks under examples/vector_add/.
Expected<std::filesystem::path>
resolveKernelPath(const Workload &workload, std::string_view basenameWithExt,
                  std::span<const std::string_view> fallbackRelative);

RunResult fillVectorAddResult(const Workload &workload, const DeviceInfo &device,
                              BackendType backend, std::string_view compilerName,
                              const ExecuteOptions &options,
                              std::span<const double> samplesMs, TimingSource timingSource,
                              std::span<const float> reference, std::span<const float> actual);

} // namespace csrun

#endif
