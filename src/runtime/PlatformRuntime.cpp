#include "csrun/runtime/PlatformRuntime.h"

#include "csrun/backend/BackendSelector.h"
#include "csrun/core/Log.h"
#include "csrun/workload/WorkloadParser.h"

#if defined(CSRUN_HAS_VULKAN)
#include "csrun/backends/vulkan/VulkanBackend.h"
#endif

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
  result.note = "inspect only";

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

Expected<RunResult> PlatformRuntime::run(const Workload &workload,
                                         std::optional<BackendType> forced) const {
  auto selection = selectBackend(workload, forced);
  if (!selection)
    return selection.error();

  Backend *backend = registry_.find(selection.value().backend);
  if (!backend)
    return Error::make(ErrorCode::BackendUnavailable, "selected backend missing from registry");

#if defined(CSRUN_HAS_VULKAN)
  if (auto *vk = dynamic_cast<VulkanBackend *>(backend))
    return vk->execute(workload, selection.value().device);
#endif

  Error e = Error::make(ErrorCode::Unimplemented,
                        std::string(toString(selection.value().backend)) +
                            " execute path is not implemented");
  e.backend = std::string(toString(selection.value().backend));
  return e;
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
