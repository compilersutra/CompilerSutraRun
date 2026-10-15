#include "csrun/runtime/PlatformRuntime.h"

#include "csrun/backend/BackendSelector.h"
#include "csrun/core/Log.h"
#include "csrun/runtime/BuiltinBackends.h"
#include "csrun/workload/WorkloadParser.h"

namespace csrun {

PlatformRuntime::PlatformRuntime() : registry_(createDefaultBackendRegistry()) {}

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
  result.note = "inspect only";

  auto selection = selectBackend(workload, forced);
  if (selection) {
    result.backend = selection.value().backend;
    result.deviceName = selection.value().device.name;
    result.deviceVendor = selection.value().device.vendor;
    result.driver = selection.value().device.driver;
    result.note = selection.value().reason;
  } else {
    if (forced)
      result.backend = *forced;
    else if (workload.preferredBackend)
      result.backend = *workload.preferredBackend;
    result.note = selection.error().toString();
    logWarn("runtime", result.note);
  }

  return result;
}

Expected<RunResult> PlatformRuntime::run(const Workload &workload,
                                         std::optional<BackendType> forced,
                                         const ExecuteOptions &options) const {
  auto selection = selectBackend(workload, forced);
  if (!selection)
    return selection.error();

  Backend *backend = registry_.find(selection.value().backend);
  if (!backend)
    return Error::make(ErrorCode::BackendUnavailable, "selected backend missing from registry");

  auto result = backend->execute(workload, selection.value().device, options);
  if (result)
    result.value().environment = gatherEnvironmentInfo();
  return result;
}

Expected<std::vector<RunResult>> PlatformRuntime::runAll(const Workload &workload,
                                                         const ExecuteOptions &options) const {
  std::vector<RunResult> results;
  for (const auto &b : registry_.backends()) {
    const BackendInfo info = b->info();
    if (!info.available)
      continue;
    auto one = run(workload, info.type, options);
    if (!one) {
      RunResult skipped;
      skipped.workloadName = workload.name;
      skipped.operation = workload.operation;
      skipped.backend = info.type;
      skipped.validation = ValidationStatus::Skipped;
      skipped.note = one.error().toString();
      results.push_back(std::move(skipped));
      continue;
    }
    results.push_back(std::move(one.value()));
  }
  if (results.empty())
    return Error::make(ErrorCode::BackendUnavailable, "no available backends for --all");
  return results;
}

std::vector<BackendInfo> PlatformRuntime::listBackends() const {
  std::vector<BackendInfo> out;
  out.reserve(registry_.backends().size());
  for (const auto &b : registry_.backends())
    out.push_back(b->info());
  return out;
}

Expected<std::vector<DeviceInfo>>
PlatformRuntime::listDevices(std::optional<BackendType> type) const {
  std::vector<DeviceInfo> all;
  for (const auto &b : registry_.backends()) {
    const auto info = b->info();
    if (type && info.type != *type)
      continue;
    if (!info.available)
      continue;
    auto devices = b->enumerateDevices();
    if (!devices)
      continue;
    for (auto &d : devices.value())
      all.push_back(std::move(d));
  }
  if (all.empty())
    return Error::make(ErrorCode::DeviceNotFound, "no devices available from enabled backends");
  return all;
}

} // namespace csrun
