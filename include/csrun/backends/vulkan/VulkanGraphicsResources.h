//===----------------------------------------------------------------------===//
//
// VulkanGraphicsResources.h — auto-bind UBO / sampler / vertex attrs for smoke runs
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKENDS_VULKAN_VULKANGRAPHICSRESOURCES_H
#define CSRUN_BACKENDS_VULKAN_VULKANGRAPHICSRESOURCES_H

#include "csrun/compiler/SpirvReflect.h"
#include "csrun/core/Expected.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace csrun {

struct VulkanGraphicsResources {
  SpirvModuleReflection reflection;

  // Contiguous layouts/sets from 0 .. maxUsedSet (empty layouts fill gaps).
  std::vector<VkDescriptorSetLayout> setLayouts;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkDescriptorPool descPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> descSets;

  std::vector<VkBuffer> uboBuffers;
  std::vector<VkDeviceMemory> uboMemories;
  std::vector<VkBuffer> ssboBuffers;
  std::vector<VkDeviceMemory> ssboMemories;

  VkImage texImage = VK_NULL_HANDLE;
  VkDeviceMemory texMemory = VK_NULL_HANDLE;
  VkImageView texView = VK_NULL_HANDLE;
  VkSampler texSampler = VK_NULL_HANDLE;

  VkBuffer vertexBuffer = VK_NULL_HANDLE;
  VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
  std::uint32_t vertexCount = 3;
  std::uint32_t vertexStride = 0;

  std::vector<VkVertexInputBindingDescription> vertexBindings;
  std::vector<VkVertexInputAttributeDescription> vertexAttrs;

  std::vector<std::uint8_t> pushBytes;
  VkShaderStageFlags pushStages = 0;

  bool hasDescriptors = false;
  bool hasVertexAttrs = false;
};

/// Build descriptor / vertex / push resources from merged reflection.
/// Fills identity matrices into UBOs and a small white texture for samplers.
Expected<VulkanGraphicsResources>
vulkanCreateGraphicsResources(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                              uint32_t queueFamily, const SpirvModuleReflection &refl,
                              std::vector<std::string> &steps);

void vulkanDestroyGraphicsResources(VkDevice device, VulkanGraphicsResources &res);

/// Bind descriptor set + vertex buffer + push constants (call inside render pass).
void vulkanCmdBindGraphicsResources(VkCommandBuffer cmd, const VulkanGraphicsResources &res);

} // namespace csrun

#endif
