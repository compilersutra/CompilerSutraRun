//===----------------------------------------------------------------------===//
// DeviceInfo.h
//===----------------------------------------------------------------------===//

#ifndef CSRUN_DEVICE_DEVICEINFO_H
#define CSRUN_DEVICE_DEVICEINFO_H

#include "csrun/backend/BackendType.h"

#include <cstdint>
#include <string>

namespace csrun {

struct DeviceInfo {
  BackendType backend{};
  std::uint32_t index = 0;
  std::string name;
  std::string vendor;
  std::string driver;
  bool available = false;
};

} // namespace csrun

#endif
