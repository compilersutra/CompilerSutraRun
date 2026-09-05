#include "csrun/runtime/PlatformRuntime.h"

#include "csrun/backend/BackendSelector.h"
#include "csrun/core/Log.h"
#include "csrun/workload/WorkloadParser.h"

namespace csrun {

PlatformRuntime::PlatformRuntime() : registry_(BackendRegistry::createDefault()) {}

Expected<Workload> PlatformRuntime::loadWorkload(const std::filesystem::path &path) const {
  return WorkloadParser::parseFile(path);
}

Expected<SelectionResult>
PlatformRuntime::selectBackend(const Workload &workload,
                               std::optional<BackendType> forced) const {
  BackendSelector selector(registry_);
  return selector.select(workload, forced);
}

Expected<RunResult> PlatformRuntime::inspect(const Workload &workload,
                                             std::optional<BackendType> forced) const {
  RunResult result;
  result.workloadName = workload.name;
  result.operation = workload.operation;
  result.warmup = workload.execution.warmup;
  result.iterations = workload.execution.iterations;
  result.validation = ValidationStatus::Skipped;
  result.note = "Phase 1 inspect only: GPU execute/validate/benchmark not implemented yet";

  if (forced)
    result.backend = *forced;
  else if (workload.preferredBackend)
    result.backend = *workload.preferredBackend;
  else
    result.backend = BackendType::Vulkan;

  auto selection = selectBackend(workload, forced);
  if (selection) {
    result.backend = selection.value().backend;
    result.deviceName = selection.value().device.name;
    result.deviceVendor = selection.value().device.vendor;
    result.driver = selection.value().device.driver;
    result.note = selection.value().reason;
  } else {
    result.note = selection.error().toString();
    logWarn("runtime", result.note);
  }

  return result;
}

std::vector<BackendInfo> PlatformRuntime::listBackends() const {
  std::vector<BackendInfo> out;
  out.reserve(registry_.backends().size());
  for (const auto &b : registry_.backends())
    out.push_back(b->info());
  return out;
}

} // namespace csrun
