//===----------------------------------------------------------------------===//
// VulkanDevicePick.h - Prefer discrete RADV + performance-query capable GPUs
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKENDS_VULKAN_VULKANDEVICEPICK_H
#define CSRUN_BACKENDS_VULKAN_VULKANDEVICEPICK_H

#include "csrun/backends/vulkan/VulkanPerfQuery.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace csrun {

struct VulkanDeviceMeta {
  uint32_t index = 0;
  std::string name;
  std::string typeName;
  std::string driverName;
  std::string driverInfo;
  VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
  VkDriverId driverId = static_cast<VkDriverId>(0);
  bool hasCompute = false;
  bool hasGraphics = false;
  bool hasPerfQuery = false;
  bool isCpu = false;
};

inline const char *vulkanDeviceTypeName(VkPhysicalDeviceType t) {
  switch (t) {
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    return "integrated";
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
    return "discrete";
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
    return "virtual";
  case VK_PHYSICAL_DEVICE_TYPE_CPU:
    return "cpu";
  default:
    return "other";
  }
}

inline VulkanDeviceMeta vulkanQueryDeviceMeta(VkPhysicalDevice physical, uint32_t index) {
  VulkanDeviceMeta m;
  m.index = index;

  VkPhysicalDeviceDriverProperties driverProps{};
  driverProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &driverProps;
  vkGetPhysicalDeviceProperties2(physical, &props2);

  m.name = props2.properties.deviceName;
  m.type = props2.properties.deviceType;
  m.typeName = vulkanDeviceTypeName(m.type);
  m.isCpu = (m.type == VK_PHYSICAL_DEVICE_TYPE_CPU);
  m.driverId = driverProps.driverID;
  m.driverName = driverProps.driverName[0] ? driverProps.driverName : "unknown";
  m.driverInfo = driverProps.driverInfo;

  uint32_t qCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qCount, nullptr);
  std::vector<VkQueueFamilyProperties> qprops(qCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qCount, qprops.data());
  for (const auto &q : qprops) {
    if (q.queueFlags & VK_QUEUE_COMPUTE_BIT)
      m.hasCompute = true;
    if (q.queueFlags & VK_QUEUE_GRAPHICS_BIT)
      m.hasGraphics = true;
  }

  m.hasPerfQuery = vulkanPreparePerfQueryDevice(physical).want;
  return m;
}

/// Higher is better. Prefers perf-query capable RADV, then discrete, then integrated.
/// csrun is a counter/benchmark tool — HW counters outweigh raw discrete preference.
inline int vulkanDeviceScore(const VulkanDeviceMeta &m) {
  int score = 0;
  switch (m.type) {
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
    score += 10000;
    break;
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    score += 5000;
    break;
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
    score += 1000;
    break;
  case VK_PHYSICAL_DEVICE_TYPE_CPU:
    score -= 10000;
    break;
  default:
    break;
  }
  // Strong weight: without this, RX 9060 XT (GFX12) wins but cannot sample AMD HW counters
  // on current Mesa, while Raphael iGPU can.
  if (m.hasPerfQuery)
    score += 12000;
  if (m.driverId == VK_DRIVER_ID_MESA_RADV)
    score += 1500;
  else if (m.driverId == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
           m.driverId == VK_DRIVER_ID_AMD_PROPRIETARY)
    score += 200;
  else if (m.driverId == VK_DRIVER_ID_MESA_LLVMPIPE)
    score -= 5000;

  const std::string &n = m.name;
  if (n.find("llvmpipe") != std::string::npos || n.find("Lavapipe") != std::string::npos)
    score -= 5000;
  return score;
}

inline uint32_t vulkanPickPhysicalDevice(const std::vector<VkPhysicalDevice> &phys,
                                         std::optional<uint32_t> requestedIndex,
                                         bool wantGraphics) {
  if (phys.empty())
    return 0;
  if (requestedIndex.has_value()) {
    if (*requestedIndex < phys.size())
      return *requestedIndex;
    return 0;
  }

  uint32_t best = 0;
  int bestScore = vulkanDeviceScore(vulkanQueryDeviceMeta(phys[0], 0));
  for (uint32_t i = 0; i < static_cast<uint32_t>(phys.size()); ++i) {
    VulkanDeviceMeta m = vulkanQueryDeviceMeta(phys[i], i);
    if (wantGraphics && !m.hasGraphics)
      continue;
    if (!wantGraphics && !m.hasCompute)
      continue;
    const int s = vulkanDeviceScore(m);
    if (s > bestScore) {
      bestScore = s;
      best = i;
    }
  }
  return best;
}

inline std::string vulkanPerfQueryHint(const VulkanDeviceMeta &m) {
  if (m.hasPerfQuery)
    return "VK_KHR_performance_query available";
  std::string s = "VK_KHR_performance_query not present on this device/driver";
  s += " (device=\"" + m.name + "\" type=" + m.typeName + " driver=\"" + m.driverName + "\")";
  if (m.isCpu || m.driverId == VK_DRIVER_ID_MESA_LLVMPIPE) {
    s += ". Only a software Vulkan device is visible — your AMD GPU is not enumerated. "
         "Check /dev/dri, amdgpu, and: "
         "DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1 "
         "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json "
         "(or /etc/vulkan/icd.d/radeon_icd_amdgpu.x86_64.json)";
  } else if (m.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
             (m.name.find("GFX1200") != std::string::npos ||
              m.name.find("9060") != std::string::npos ||
              m.name.find("Navi 4") != std::string::npos)) {
    s += ". RADV on RDNA4/GFX12 often still lacks performanceCounterQueryPools in current Mesa. "
         "Use the integrated RADV device for HW counters, e.g. --device-index 1, or wait for "
         "newer Mesa RADV perf-counter support on GFX12.";
  } else if (m.driverId == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
             m.driverId == VK_DRIVER_ID_AMD_PROPRIETARY ||
             m.driverName.find("AMD") != std::string::npos) {
    s += ". AMDVLK/proprietary often lacks this extension — force Mesa RADV: "
         "DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1 "
         "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json";
  } else {
    s += ". For AMD HW counters use Mesa RADV on a device with perf_query=yes "
         "(see csrun --list-devices).";
  }
  return s;
}

/// If the picked device lacks perf query but another enumerated device has it, return a hint.
inline std::string vulkanSuggestPerfQueryDevice(const std::vector<VkPhysicalDevice> &phys,
                                                uint32_t pickedIndex) {
  if (pickedIndex >= phys.size())
    return {};
  VulkanDeviceMeta picked = vulkanQueryDeviceMeta(phys[pickedIndex], pickedIndex);
  if (picked.hasPerfQuery)
    return {};
  for (uint32_t i = 0; i < static_cast<uint32_t>(phys.size()); ++i) {
    if (i == pickedIndex)
      continue;
    VulkanDeviceMeta m = vulkanQueryDeviceMeta(phys[i], i);
    if (m.hasPerfQuery) {
      return "note: device[" + std::to_string(pickedIndex) + "] has no HW perf counters; "
             "device[" + std::to_string(i) + "] \"" + m.name +
             "\" has perf_query=yes — pass --device-index " + std::to_string(i) +
             " (or rely on auto-pick which prefers perf_query)";
    }
  }
  return {};
}

} // namespace csrun

#endif
