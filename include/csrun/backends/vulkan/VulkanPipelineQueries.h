//===----------------------------------------------------------------------===//
// VulkanPipelineQueries.h - Shared Vulkan pipeline-stat helpers
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKENDS_VULKAN_VULKANPIPELINEQUERIES_H
#define CSRUN_BACKENDS_VULKAN_VULKANPIPELINEQUERIES_H

#include "csrun/benchmark/PipelineStats.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace csrun {

inline VkQueryPipelineStatisticFlags graphicsPipelineStatFlags() {
  return VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
         VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
         VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
         VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
         VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
         VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
         VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
         VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
         VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT |
         VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT;
}

inline VkQueryPipelineStatisticFlags computePipelineStatFlags() {
  return VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
}

inline uint32_t countPipelineStatBits(VkQueryPipelineStatisticFlags flags) {
  uint32_t n = 0;
  for (uint32_t bit = 0; bit < 32; ++bit) {
    if (flags & (1u << bit))
      ++n;
  }
  return n;
}

inline PipelineStats parsePipelineStats(VkQueryPipelineStatisticFlags flags, const uint64_t *data,
                                        uint32_t count) {
  PipelineStats s;
  s.available = true;
  uint32_t idx = 0;
  auto take = [&](VkQueryPipelineStatisticFlagBits bit, uint64_t &dst) {
    if (!(flags & bit))
      return;
    if (idx < count)
      dst = data[idx];
    ++idx;
  };
  take(VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT, s.iaVertices);
  take(VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT, s.iaPrimitives);
  take(VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT, s.vsInvocations);
  take(VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT, s.gsInvocations);
  take(VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT, s.gsPrimitives);
  take(VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT, s.clipInvocations);
  take(VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT, s.clipPrimitives);
  take(VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT, s.fsInvocations);
  take(VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT, s.tcsPatches);
  take(VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT, s.tesInvocations);
  take(VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT, s.csInvocations);
  return s;
}

/// Fetch 64-bit query results. Uses WAIT after a soft NOT_READY; returns false if still unavailable
/// (caller should soft-fail — do not abort the whole run).
inline bool vulkanGetQueryResultsU64(VkDevice device, VkQueryPool pool, uint32_t firstQuery,
                                     uint32_t queryCount, uint64_t *data, VkDeviceSize stride) {
  if (!pool || !data || queryCount == 0)
    return false;
  const VkDeviceSize bytes = stride * queryCount;
  VkResult r = vkGetQueryPoolResults(device, pool, firstQuery, queryCount, bytes, data, stride,
                                     VK_QUERY_RESULT_64_BIT);
  if (r == VK_NOT_READY) {
    vkDeviceWaitIdle(device);
    r = vkGetQueryPoolResults(device, pool, firstQuery, queryCount, bytes, data, stride,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  }
  // VK_SUCCESS (0). Some drivers may still return NOT_READY without hang; treat as miss.
  return r == VK_SUCCESS;
}

} // namespace csrun

#endif
