#include "csrun/workload/VectorAddScaffold.h"

#include "csrun/benchmark/Statistics.h"
#include "csrun/validation/Comparator.h"

#include <string>

namespace csrun {

Expected<std::uint32_t> requireVectorAddFp32(const Workload &workload,
                                            std::string_view backendName) {
  if (workload.operation != "vector_add") {
    Error e = Error::make(ErrorCode::UnsupportedOperation,
                          std::string(backendName) + " supports vector_add only; got " +
                              workload.operation);
    e.backend = std::string(backendName);
    return e;
  }
  if (workload.inputDtype != DataType::FP32 || workload.outputDtype != DataType::FP32) {
    Error e = Error::make(ErrorCode::UnsupportedDtype,
                          std::string(backendName) + " vector_add requires fp32");
    e.backend = std::string(backendName);
    return e;
  }
  const auto n = static_cast<std::uint32_t>(workload.dimensions.count);
  if (n == 0) {
    Error e = Error::make(ErrorCode::InvalidWorkload, "vector_add requires dimensions.count > 0");
    e.backend = std::string(backendName);
    return e;
  }
  return n;
}

VectorAddHost makeVectorAddHost(std::uint32_t count) {
  VectorAddHost host;
  host.count = count;
  host.a.resize(count);
  host.b.resize(count);
  host.reference.resize(count);
  host.output.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    host.a[i] = static_cast<float>(i) * 0.5f;
    host.b[i] = static_cast<float>(i) * 0.25f + 1.0f;
    host.reference[i] = host.a[i] + host.b[i];
  }
  return host;
}

Expected<std::filesystem::path>
resolveKernelPath(const Workload &workload, std::string_view basenameWithExt,
                  std::span<const std::string_view> fallbackRelative) {
  namespace fs = std::filesystem;
  const std::string base(basenameWithExt);

  if (!workload.sourcePath.empty()) {
    const fs::path dir = fs::path(workload.sourcePath).parent_path();
    const fs::path candidates[] = {
        dir / base,
        dir / (workload.operation + fs::path(base).extension().string()),
        dir / (workload.name + fs::path(base).extension().string()),
        dir / "kernels" / base,
    };
    for (const auto &c : candidates) {
      if (fs::exists(c))
        return c;
    }
  }

  for (const auto rel : fallbackRelative) {
    if (fs::exists(rel))
      return fs::path(rel);
  }

  return Error::make(ErrorCode::IoError, "kernel not found: " + base);
}

RunResult fillVectorAddResult(const Workload &workload, const DeviceInfo &device,
                              BackendType backend, std::string_view compilerName,
                              const ExecuteOptions &options,
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
