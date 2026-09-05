//===----------------------------------------------------------------------===//
// Backend.h - Narrow backend facade interfaces
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKEND_BACKEND_H
#define CSRUN_BACKEND_BACKEND_H

#include "csrun/backend/BackendType.h"
#include "csrun/core/Expected.h"
#include "csrun/device/DeviceInfo.h"

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
};

} // namespace csrun

#endif
