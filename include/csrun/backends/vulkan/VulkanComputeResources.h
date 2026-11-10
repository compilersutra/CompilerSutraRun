//===----------------------------------------------------------------------===//
//
// VulkanComputeResources.h — reflect-driven compute smoke (UBO / SSBO / storage image)
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKENDS_VULKAN_VULKANCOMPUTERESOURCES_H
#define CSRUN_BACKENDS_VULKAN_VULKANCOMPUTERESOURCES_H

#include "csrun/backends/vulkan/VulkanSpirvRunner.h"
#include "csrun/compiler/SpirvReflect.h"
#include "csrun/core/Expected.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace csrun {

/// True when compute needs more than the legacy 3-SSBO vector_add/matmul path.
bool vulkanComputeNeedsGenericResources(const SpirvModuleReflection &refl);

/// Dispatch compute using reflected UBO / SSBO / storage-image bindings (set 0..N).
/// Caller owns instance/device lifetime; this creates and destroys its own modules/resources.
Expected<void> vulkanRunComputeGeneric(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                                       uint32_t queueFamily, const std::vector<uint32_t> &words,
                                       const VulkanSmokeRequest &req, VulkanSmokeResult &out);

} // namespace csrun

#endif
