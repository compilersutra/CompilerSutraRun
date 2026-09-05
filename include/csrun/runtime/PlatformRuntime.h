//===----------------------------------------------------------------------===//
// PlatformRuntime.h - CLI-facing facade
//===----------------------------------------------------------------------===//

#ifndef CSRUN_RUNTIME_PLATFORMRUNTIME_H
#define CSRUN_RUNTIME_PLATFORMRUNTIME_H

#include "csrun/backend/BackendRegistry.h"
#include "csrun/backend/BackendSelector.h"
#include "csrun/core/Expected.h"
#include "csrun/device/DeviceInfo.h"
#include "csrun/result/RunResult.h"
#include "csrun/workload/Workload.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace csrun {

class PlatformRuntime {
public:
  PlatformRuntime();

  const BackendRegistry &registry() const { return registry_; }

  Expected<Workload> loadWorkload(const std::filesystem::path &path) const;
  Expected<SelectionResult> selectBackend(const Workload &workload,
                                          std::optional<BackendType> forced) const;

  Expected<RunResult> inspect(const Workload &workload,
                              std::optional<BackendType> forced) const;

  /// Select backend, execute, validate, and benchmark when supported.
  Expected<RunResult> run(const Workload &workload,
                          std::optional<BackendType> forced) const;

  std::vector<BackendInfo> listBackends() const;
  Expected<std::vector<DeviceInfo>> listDevices(std::optional<BackendType> type) const;

private:
  BackendRegistry registry_;
};

} // namespace csrun

#endif
