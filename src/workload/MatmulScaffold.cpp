#include "csrun/workload/MatmulScaffold.h"

#include "csrun/benchmark/Statistics.h"
#include "csrun/validation/Comparator.h"

namespace csrun {

Expected<MatmulDims> requireMatmulFp32(const Workload &workload, std::string_view backendName) {
  if (workload.operation != "matmul") {
    Error e = Error::make(ErrorCode::UnsupportedOperation,
                          std::string(backendName) + " matmul path requires operation matmul; got " +
                              workload.operation);
    e.backend = std::string(backendName);
    return e;
  }
  if (workload.inputDtype != DataType::FP32 || workload.outputDtype != DataType::FP32) {
    Error e = Error::make(ErrorCode::UnsupportedDtype,
                          std::string(backendName) + " matmul requires fp32");
    e.backend = std::string(backendName);
    return e;
  }
  MatmulDims d;
  d.M = static_cast<std::uint32_t>(workload.dimensions.m);
  d.N = static_cast<std::uint32_t>(workload.dimensions.n);
  d.K = static_cast<std::uint32_t>(workload.dimensions.k);
  if (d.M == 0 || d.N == 0 || d.K == 0) {
    Error e = Error::make(ErrorCode::InvalidWorkload,
                          "matmul requires dimensions.M/N/K > 0");
    e.backend = std::string(backendName);
    return e;
  }
  return d;
}

MatmulHost makeMatmulHost(MatmulDims dims) {
  MatmulHost host;
  host.dims = dims;
  const std::size_t aN = static_cast<std::size_t>(dims.M) * dims.K;
  const std::size_t bN = static_cast<std::size_t>(dims.K) * dims.N;
  const std::size_t cN = static_cast<std::size_t>(dims.M) * dims.N;
  host.a.resize(aN);
  host.b.resize(bN);
  host.reference.resize(cN);
  host.output.assign(cN, 0.f);
  for (std::uint32_t i = 0; i < dims.M; ++i) {
    for (std::uint32_t k = 0; k < dims.K; ++k)
      host.a[static_cast<std::size_t>(i) * dims.K + k] =
          0.01f * static_cast<float>(i + 1) + 0.001f * static_cast<float>(k + 1);
  }
  for (std::uint32_t k = 0; k < dims.K; ++k) {
    for (std::uint32_t j = 0; j < dims.N; ++j)
      host.b[static_cast<std::size_t>(k) * dims.N + j] =
          0.02f * static_cast<float>(k + 1) + 0.0005f * static_cast<float>(j + 1);
  }
  for (std::uint32_t i = 0; i < dims.M; ++i) {
    for (std::uint32_t j = 0; j < dims.N; ++j) {
      float acc = 0.f;
      for (std::uint32_t k = 0; k < dims.K; ++k)
        acc += host.a[static_cast<std::size_t>(i) * dims.K + k] *
               host.b[static_cast<std::size_t>(k) * dims.N + j];
      host.reference[static_cast<std::size_t>(i) * dims.N + j] = acc;
    }
  }
  return host;
}

RunResult fillMatmulResult(const Workload &workload, const DeviceInfo &device, BackendType backend,
                           std::string_view compilerName, const ExecuteOptions &options,
                           std::span<const double> samplesMs, TimingSource timingSource,
                           std::span<const float> reference, std::span<const float> actual) {
  RunResult result;
  result.workloadName = workload.name;
  result.operation = workload.operation;
  result.backend = backend;
  result.deviceName = device.name;
  result.deviceVendor = device.vendor;
  result.driver = device.driver;
  result.compilerName = std::string(compilerName);
  result.warmup = workload.execution.warmup;
  result.iterations = options.benchmark ? workload.execution.iterations : 1;
  result.timing = Statistics::compute(samplesMs, timingSource);

  const bool doValidate = options.validate && workload.validation.enabled;
  if (!doValidate) {
    result.validation = ValidationStatus::Skipped;
    result.note = "validation skipped";
    return result;
  }

  ComparePolicy policy;
  policy.absoluteTolerance = workload.validation.absoluteTolerance;
  policy.relativeTolerance = workload.validation.relativeTolerance;
  const CompareResult cmp = Comparator::compareFloat32(reference, actual, policy);
  result.validation = cmp.passed ? ValidationStatus::Passed : ValidationStatus::Failed;
  result.note = cmp.message;
  return result;
}

} // namespace csrun
