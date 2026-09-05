//===----------------------------------------------------------------------===//
// VulkanBackend.h
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKENDS_VULKAN_VULKANBACKEND_H
#define CSRUN_BACKENDS_VULKAN_VULKANBACKEND_H

#include "csrun/backend/Backend.h"
#include "csrun/result/RunResult.h"
#include "csrun/workload/Workload.h"

#include <memory>

namespace csrun {

class VulkanBackend final : public Backend {
public:
  VulkanBackend();
  ~VulkanBackend() override;

  BackendInfo info() const override;
  Expected<std::vector<DeviceInfo>> enumerateDevices() const override;

  /// Execute a supported compute workload on the selected device.
  Expected<RunResult> execute(const Workload &workload, const DeviceInfo &device) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::unique_ptr<Backend> createVulkanBackend();

} // namespace csrun

#endif
