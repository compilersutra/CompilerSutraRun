//===----------------------------------------------------------------------===//
// MatmulScaffold.h - Shared host prep for C = A*B (row-major fp32)
//===----------------------------------------------------------------------===//

#ifndef CSRUN_WORKLOAD_MATMULSCAFFOLD_H
#define CSRUN_WORKLOAD_MATMULSCAFFOLD_H

#include "csrun/backend/BackendType.h"
#include "csrun/backend/ExecuteOptions.h"
#include "csrun/benchmark/Timing.h"
#include "csrun/core/Expected.h"
#include "csrun/device/DeviceInfo.h"
#include "csrun/result/RunResult.h"
#include "csrun/workload/Workload.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace csrun {

struct MatmulDims {
  std::uint32_t M = 0;
  std::uint32_t N = 0;
  std::uint32_t K = 0;
};

struct MatmulHost {
  MatmulDims dims;
  std::vector<float> a; // M*K
  std::vector<float> b; // K*N
  std::vector<float> reference; // M*N
  std::vector<float> output;    // M*N
};

Expected<MatmulDims> requireMatmulFp32(const Workload &workload, std::string_view backendName);

MatmulHost makeMatmulHost(MatmulDims dims);

RunResult fillMatmulResult(const Workload &workload, const DeviceInfo &device, BackendType backend,
                           std::string_view compilerName, const ExecuteOptions &options,
                           std::span<const double> samplesMs, TimingSource timingSource,
                           std::span<const float> reference, std::span<const float> actual);

} // namespace csrun

#endif
