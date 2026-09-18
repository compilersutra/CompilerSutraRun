//===----------------------------------------------------------------------===//
// Backend.h - Narrow backend facade
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKEND_BACKEND_H
#define CSRUN_BACKEND_BACKEND_H

#include "csrun/backend/BackendType.h"
#include "csrun/backend/ExecuteOptions.h"
#include "csrun/core/Expected.h"
#include "csrun/device/DeviceInfo.h"
#include "csrun/result/RunResult.h"
#include "csrun/workload/Workload.h"

#include <string>
#include <vector>

namespace csrun {

struct BackendInfo {
  BackendType type{};
  std::string name;
  bool available = false;
  std::string availabilityNote;
};

class Backend {
public:
  virtual ~Backend() = default;

  virtual BackendInfo info() const = 0;
  virtual Expected<std::vector<DeviceInfo>> enumerateDevices() const = 0;

  /// Execute workload on device. Stubs return Unimplemented / Unavailable.
  virtual Expected<RunResult> execute(const Workload &workload, const DeviceInfo &device,
                                      const ExecuteOptions &options) const {
    (void)workload;
    (void)device;
    (void)options;
    Error e = Error::make(ErrorCode::Unimplemented,
                          std::string(toString(info().type)) + " execute path is not implemented");
    e.backend = std::string(toString(info().type));
    return e;
  }
};

} // namespace csrun

#endif
