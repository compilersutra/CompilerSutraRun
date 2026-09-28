//===----------------------------------------------------------------------===//
//
// VulkanRayTracingSmoke.h — minimal RT pipeline smoke (rgen/rchit/rmiss)
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKENDS_VULKAN_VULKANRAYTRACINGSMOKE_H
#define CSRUN_BACKENDS_VULKAN_VULKANRAYTRACINGSMOKE_H

#include "csrun/backends/vulkan/VulkanSpirvRunner.h"

#include <filesystem>

namespace csrun {

struct VulkanRayTracingRequest {
  std::filesystem::path raygenSpv;
  std::filesystem::path closestHitSpv;
  std::filesystem::path missSpv;
  std::optional<std::uint32_t> deviceIndex;
  std::uint32_t width = 64;
  std::uint32_t height = 64;
  int warmup = 0;
  int iterations = 1;
};

/// Offscreen ray-tracing smoke: triangle BLAS/TLAS + traceRays + image readback.
Expected<VulkanSmokeResult> vulkanRayTracingSmoke(const VulkanRayTracingRequest &req);

} // namespace csrun

#endif
