//===----------------------------------------------------------------------===//
// VulkanPerfQuery.h - VK_KHR_performance_query (RADV AMD HW counters)
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKENDS_VULKAN_VULKANPERFQUERY_H
#define CSRUN_BACKENDS_VULKAN_VULKANPERFQUERY_H

#include "csrun/benchmark/PipelineStats.h"
#include "csrun/benchmark/VendorCountersMode.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace csrun {

struct VulkanPerfCounterDesc {
  uint32_t index = 0;
  std::string name;
  std::string category;
  std::string description;
  std::string unit;
  VkPerformanceCounterStorageKHR storage = VK_PERFORMANCE_COUNTER_STORAGE_FLOAT64_KHR;
};

struct VulkanPerfQuerySession {
  bool available = false;
  std::string note;
  uint32_t queueFamily = 0;
  uint32_t passCount = 1;
  VkQueryPool pool = VK_NULL_HANDLE;
  std::vector<VulkanPerfCounterDesc> enabled;
  std::vector<uint32_t> enabledIndices;
  bool profilingLockHeld = false;
};

bool vulkanHasDeviceExtension(VkPhysicalDevice physical, const char *name);

struct VulkanPerfDeviceSetup {
  bool want = false;
  VkPhysicalDevicePerformanceQueryFeaturesKHR features{};
};

using VulkanPerfCounterMode = VendorCountersMode;

VulkanPerfDeviceSetup vulkanPreparePerfQueryDevice(VkPhysicalDevice physical);

VulkanPerfQuerySession vulkanCreatePerfQuerySession(
    VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily,
    std::vector<std::string> &steps,
    VendorCountersMode mode = VendorCountersMode::PreferSinglePass);

void vulkanDestroyPerfQuerySession(VkDevice device, VulkanPerfQuerySession &session);

void vulkanPerfCmdReset(VkCommandBuffer cmd, const VulkanPerfQuerySession &session);
void vulkanPerfCmdBegin(VkCommandBuffer cmd, const VulkanPerfQuerySession &session);
void vulkanPerfCmdEnd(VkCommandBuffer cmd, const VulkanPerfQuerySession &session);

bool vulkanPerfQueryResults(VkDevice device, const VulkanPerfQuerySession &session,
                            std::vector<VendorCounter> &out, std::string &outNote);

} // namespace csrun

#endif
