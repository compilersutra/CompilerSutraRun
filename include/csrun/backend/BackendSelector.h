//===----------------------------------------------------------------------===//
// BackendSelector.h
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKEND_BACKENDSELECTOR_H
#define CSRUN_BACKEND_BACKENDSELECTOR_H

#include "csrun/backend/BackendRegistry.h"
#include "csrun/core/Expected.h"
#include "csrun/device/DeviceInfo.h"
#include "csrun/workload/Workload.h"

#include <optional>

namespace csrun {

struct SelectionResult {
  BackendType backend{};
  DeviceInfo device;
  std::string reason;
};

class BackendSelector {
public:
  explicit BackendSelector(const BackendRegistry &registry);

  Expected<SelectionResult> select(const Workload &workload,
                                   std::optional<BackendType> forced) const;

private:
  const BackendRegistry &registry_;
};

} // namespace csrun

#endif
