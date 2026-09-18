//===----------------------------------------------------------------------===//
//
// VulkanSpirvRunner.h
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKENDS_VULKAN_VULKANSPIRVRUNNER_H
#define CSRUN_BACKENDS_VULKAN_VULKANSPIRVRUNNER_H

#include "csrun/compiler/SpirvToolchain.h"
#include "csrun/core/Expected.h"
#include "csrun/device/DeviceInfo.h"

#include <filesystem>
#include <string>
#include <vector>

namespace csrun {

struct VulkanSmokeRequest {
  std::filesystem::path computeSpv;
  std::filesystem::path vertexSpv;
  std::filesystem::path fragmentSpv;
  std::uint32_t deviceIndex = 0;
};

struct VulkanSmokeResult {
  std::string deviceName;
  std::string mode;
  std::vector<std::string> steps;
  bool ok = false;
};

Expected<VulkanSmokeResult> vulkanSmokeSpirv(const VulkanSmokeRequest &req);

} // namespace csrun

#endif
