//===----------------------------------------------------------------------===//
// PlatformRuntime.h - CLI-facing facade
//===----------------------------------------------------------------------===//

#ifndef CSRUN_RUNTIME_PLATFORMRUNTIME_H
#define CSRUN_RUNTIME_PLATFORMRUNTIME_H

#include "csrun/backend/BackendRegistry.h"
#include "csrun/backend/BackendSelector.h"
#include "csrun/core/Expected.h"
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

  /// Phase 1: inspect / report only. GPU execute arrives in Phase 2.
  Expected<RunResult> inspect(const Workload &workload,
                              std::optional<BackendType> forced) const;

  std::vector<BackendInfo> listBackends() const;

private:
  BackendRegistry registry_;
};

} // namespace csrun

#endif
