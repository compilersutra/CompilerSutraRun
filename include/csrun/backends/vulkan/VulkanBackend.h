//===----------------------------------------------------------------------===//
//
// VulkanBackend.h
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKENDS_VULKAN_VULKANBACKEND_H
#define CSRUN_BACKENDS_VULKAN_VULKANBACKEND_H

#include "csrun/backend/Backend.h"

#include <memory>

namespace csrun {

class VulkanBackend final : public Backend {
public:
  BackendInfo info() const override;
  Expected<std::vector<DeviceInfo>> enumerateDevices() const override;
  Expected<RunResult> execute(const Workload &workload, const DeviceInfo &device,
                              const ExecuteOptions &options) const override;
};

std::unique_ptr<Backend> createVulkanBackend();

} // namespace csrun

#endif
