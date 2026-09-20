#include "csrun/backends/vulkan/VulkanPerfQuery.h"

#include <algorithm>
#include <cstring>

namespace csrun {
namespace {

std::string unitString(VkPerformanceCounterUnitKHR u) {
  switch (u) {
  case VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR:
    return "generic";
  case VK_PERFORMANCE_COUNTER_UNIT_PERCENTAGE_KHR:
    return "%";
  case VK_PERFORMANCE_COUNTER_UNIT_NANOSECONDS_KHR:
    return "ns";
  case VK_PERFORMANCE_COUNTER_UNIT_BYTES_KHR:
    return "bytes";
  case VK_PERFORMANCE_COUNTER_UNIT_BYTES_PER_SECOND_KHR:
    return "B/s";
  case VK_PERFORMANCE_COUNTER_UNIT_KELVIN_KHR:
    return "K";
  case VK_PERFORMANCE_COUNTER_UNIT_WATTS_KHR:
    return "W";
  case VK_PERFORMANCE_COUNTER_UNIT_VOLTS_KHR:
    return "V";
  case VK_PERFORMANCE_COUNTER_UNIT_AMPS_KHR:
    return "A";
  case VK_PERFORMANCE_COUNTER_UNIT_HERTZ_KHR:
    return "Hz";
  case VK_PERFORMANCE_COUNTER_UNIT_CYCLES_KHR:
    return "cycles";
  default:
    return "";
  }
}

double counterToDouble(VkPerformanceCounterResultKHR r, VkPerformanceCounterStorageKHR storage) {
  switch (storage) {
  case VK_PERFORMANCE_COUNTER_STORAGE_INT32_KHR:
    return static_cast<double>(r.int32);
  case VK_PERFORMANCE_COUNTER_STORAGE_INT64_KHR:
    return static_cast<double>(r.int64);
  case VK_PERFORMANCE_COUNTER_STORAGE_UINT32_KHR:
    return static_cast<double>(r.uint32);
  case VK_PERFORMANCE_COUNTER_STORAGE_UINT64_KHR:
    return static_cast<double>(r.uint64);
  case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT32_KHR:
    return static_cast<double>(r.float32);
  case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT64_KHR:
    return r.float64;
  default:
    return 0.0;
  }
}

uint32_t queryPasses(PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR getPasses,
                     VkPhysicalDevice physical, uint32_t queueFamily,
                     const std::vector<uint32_t> &indices) {
  if (!getPasses || indices.empty())
    return 1;
  VkQueryPoolPerformanceCreateInfoKHR perfCi{};
  perfCi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_CREATE_INFO_KHR;
  perfCi.queueFamilyIndex = queueFamily;
  perfCi.counterIndexCount = static_cast<uint32_t>(indices.size());
  perfCi.pCounterIndices = indices.data();
  uint32_t passes = 1;
  getPasses(physical, &perfCi, &passes);
  return passes == 0 ? 1 : passes;
}

/// Largest prefix of [0..n) that fits in a single performance-query pass.
std::vector<uint32_t>
largestSinglePassPrefix(PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR getPasses,
                        VkPhysicalDevice physical, uint32_t queueFamily, uint32_t counterCount) {
  std::vector<uint32_t> best;
  if (counterCount == 0)
    return best;
  uint32_t lo = 1, hi = counterCount, ans = 1;
  while (lo <= hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    std::vector<uint32_t> idx(mid);
    for (uint32_t i = 0; i < mid; ++i)
      idx[i] = i;
    if (queryPasses(getPasses, physical, queueFamily, idx) == 1) {
      ans = mid;
      lo = mid + 1;
    } else {
      if (mid == 0)
        break;
      hi = mid - 1;
    }
  }
  best.resize(ans);
  for (uint32_t i = 0; i < ans; ++i)
    best[i] = i;
  return best;
}

} // namespace

bool vulkanHasDeviceExtension(VkPhysicalDevice physical, const char *name) {
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> exts(n);
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, exts.data());
  for (const auto &e : exts) {
    if (std::strcmp(e.extensionName, name) == 0)
      return true;
  }
  return false;
}

VulkanPerfDeviceSetup vulkanPreparePerfQueryDevice(VkPhysicalDevice physical) {
  VulkanPerfDeviceSetup out;
  out.features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR;
  if (!vulkanHasDeviceExtension(physical, VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME))
    return out;

  VkPhysicalDevicePerformanceQueryFeaturesKHR asked{};
  asked.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR;
  VkPhysicalDeviceFeatures2 feats2{};
  feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  feats2.pNext = &asked;
  vkGetPhysicalDeviceFeatures2(physical, &feats2);
  if (!asked.performanceCounterQueryPools)
    return out;

  out.want = true;
  out.features = asked;
  out.features.performanceCounterQueryPools = VK_TRUE;
  return out;
}

VulkanPerfQuerySession vulkanCreatePerfQuerySession(VkInstance instance, VkPhysicalDevice physical,
                                                    VkDevice device, uint32_t queueFamily,
                                                    std::vector<std::string> &steps,
                                                    VendorCountersMode mode) {
  VulkanPerfQuerySession session;
  session.queueFamily = queueFamily;

  if (mode == VendorCountersMode::Off) {
    session.note = "vendor HW counters disabled (--vendor-counters off)";
    steps.push_back("vendor HW counters: " + session.note);
    return session;
  }

  if (!vulkanHasDeviceExtension(physical, VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME)) {
    session.note = "VK_KHR_performance_query not present (need RADV/AMD discrete GPU)";
    steps.push_back("vendor HW counters: " + session.note);
    return session;
  }

  auto enumerate =
      reinterpret_cast<PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR>(
          vkGetInstanceProcAddr(instance,
                                "vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR"));
  if (!enumerate) {
    session.note = "failed to resolve performance-query enumerate entrypoint";
    steps.push_back("vendor HW counters: " + session.note);
    return session;
  }

  uint32_t counterCount = 0;
  VkResult r = enumerate(physical, queueFamily, &counterCount, nullptr, nullptr);
  if (r != VK_SUCCESS || counterCount == 0) {
    session.note = "no performance counters on this queue family (RADV exposes GENERAL only)";
    steps.push_back("vendor HW counters: " + session.note);
    return session;
  }

  std::vector<VkPerformanceCounterKHR> counters(counterCount);
  std::vector<VkPerformanceCounterDescriptionKHR> descs(counterCount);
  for (auto &c : counters) {
    c.sType = VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_KHR;
    c.pNext = nullptr;
  }
  for (auto &d : descs) {
    d.sType = VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_KHR;
    d.pNext = nullptr;
  }
  r = enumerate(physical, queueFamily, &counterCount, counters.data(), descs.data());
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
    session.note = "enumerate performance counters failed";
    steps.push_back("vendor HW counters: " + session.note);
    return session;
  }

  auto getPasses = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR>(
      vkGetInstanceProcAddr(instance,
                            "vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR"));

  std::vector<uint32_t> selected;
  selected.reserve(counterCount);
  if (mode == VendorCountersMode::PreferSinglePass) {
    selected = largestSinglePassPrefix(getPasses, physical, queueFamily, counterCount);
  } else {
    // Default: enable every counter RADV exposes (GFX12 all-14 needs 2 passes).
    for (uint32_t i = 0; i < counterCount; ++i)
      selected.push_back(i);
  }
  if (selected.empty()) {
    session.note = "no counters selected";
    steps.push_back("vendor HW counters: " + session.note);
    return session;
  }

  session.passCount = queryPasses(getPasses, physical, queueFamily, selected);

  // Safety: if "all" somehow needs many passes, fall back to largest single-pass set.
  if (mode == VendorCountersMode::All && session.passCount > 4) {
    selected = largestSinglePassPrefix(getPasses, physical, queueFamily, counterCount);
    session.passCount = queryPasses(getPasses, physical, queueFamily, selected);
    steps.push_back("vendor HW counters: passCount>4 — fell back to single-pass subset (" +
                    std::to_string(selected.size()) + ")");
  }

  VkQueryPoolPerformanceCreateInfoKHR perfCi{};
  perfCi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_CREATE_INFO_KHR;
  perfCi.queueFamilyIndex = queueFamily;
  perfCi.counterIndexCount = static_cast<uint32_t>(selected.size());
  perfCi.pCounterIndices = selected.data();

  VkQueryPoolCreateInfo qpci{};
  qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  qpci.pNext = &perfCi;
  qpci.queryType = VK_QUERY_TYPE_PERFORMANCE_QUERY_KHR;
  qpci.queryCount = 1;
  r = vkCreateQueryPool(device, &qpci, nullptr, &session.pool);
  if (r != VK_SUCCESS) {
    session.note = "vkCreateQueryPool(PERFORMANCE_QUERY) failed";
    steps.push_back("vendor HW counters: " + session.note);
    return session;
  }

  session.enabledIndices = selected;
  session.enabled.reserve(selected.size());
  for (uint32_t idx : selected) {
    VulkanPerfCounterDesc d;
    d.index = idx;
    d.name = descs[idx].name;
    d.category = descs[idx].category;
    d.description = descs[idx].description;
    d.unit = unitString(counters[idx].unit);
    d.storage = counters[idx].storage;
    session.enabled.push_back(std::move(d));
  }

  auto acquire = reinterpret_cast<PFN_vkAcquireProfilingLockKHR>(
      vkGetDeviceProcAddr(device, "vkAcquireProfilingLockKHR"));
  if (acquire) {
    VkAcquireProfilingLockInfoKHR lockInfo{};
    lockInfo.sType = VK_STRUCTURE_TYPE_ACQUIRE_PROFILING_LOCK_INFO_KHR;
    lockInfo.timeout = UINT64_MAX;
    if (acquire(device, &lockInfo) == VK_SUCCESS)
      session.profilingLockHeld = true;
  }

  session.available = true;
  session.note = "VK_KHR_performance_query enabled (" + std::to_string(session.enabled.size()) +
                 "/" + std::to_string(counterCount) + " counters, " +
                 std::to_string(session.passCount) + " pass(es)" +
                 (mode == VendorCountersMode::PreferSinglePass ? ", single-pass" : ", all") +
                 ")";
  steps.push_back("vendor HW counters: " + session.note);
  return session;
}

void vulkanDestroyPerfQuerySession(VkDevice device, VulkanPerfQuerySession &session) {
  if (session.profilingLockHeld) {
    auto release = reinterpret_cast<PFN_vkReleaseProfilingLockKHR>(
        vkGetDeviceProcAddr(device, "vkReleaseProfilingLockKHR"));
    if (release)
      release(device);
    session.profilingLockHeld = false;
  }
  if (session.pool) {
    vkDestroyQueryPool(device, session.pool, nullptr);
    session.pool = VK_NULL_HANDLE;
  }
  session.available = false;
}

void vulkanPerfCmdReset(VkCommandBuffer cmd, const VulkanPerfQuerySession &session) {
  if (session.available && session.pool)
    vkCmdResetQueryPool(cmd, session.pool, 0, 1);
}

void vulkanPerfCmdBegin(VkCommandBuffer cmd, const VulkanPerfQuerySession &session) {
  if (session.available && session.pool)
    vkCmdBeginQuery(cmd, session.pool, 0, 0);
}

void vulkanPerfCmdEnd(VkCommandBuffer cmd, const VulkanPerfQuerySession &session) {
  if (session.available && session.pool)
    vkCmdEndQuery(cmd, session.pool, 0);
}

bool vulkanPerfQueryResults(VkDevice device, const VulkanPerfQuerySession &session,
                            std::vector<VendorCounter> &out, std::string &outNote) {
  out.clear();
  if (!session.available || !session.pool) {
    outNote = session.note.empty() ? "performance query unavailable" : session.note;
    return false;
  }
  const size_t n = session.enabled.size();
  std::vector<VkPerformanceCounterResultKHR> results(n);
  VkResult r =
      vkGetQueryPoolResults(device, session.pool, 0, 1, results.size() * sizeof(results[0]),
                            results.data(), sizeof(results[0]), 0);
  if (r == VK_NOT_READY) {
    vkDeviceWaitIdle(device);
    r = vkGetQueryPoolResults(device, session.pool, 0, 1, results.size() * sizeof(results[0]),
                              results.data(), sizeof(results[0]), VK_QUERY_RESULT_WAIT_BIT);
  }
  if (r != VK_SUCCESS) {
    outNote = "vkGetQueryPoolResults(PERFORMANCE_QUERY) failed (VkResult=" +
              std::to_string(static_cast<int>(r)) + ")";
    return false;
  }
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    VendorCounter vc;
    vc.name = session.enabled[i].name;
    vc.category = session.enabled[i].category;
    vc.description = session.enabled[i].description;
    vc.unit = session.enabled[i].unit;
    vc.value = counterToDouble(results[i], session.enabled[i].storage);
    out.push_back(std::move(vc));
  }
  outNote = session.note;
  return true;
}

} // namespace csrun
