//===----------------------------------------------------------------------===//
// VulkanWsiPresenter — industry-grade Vulkan WSI (GLFW surface + swapchain)
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKENDS_VULKAN_VULKANWSIPRESENTER_H
#define CSRUN_BACKENDS_VULKAN_VULKANWSIPRESENTER_H

#include "csrun/benchmark/PipelineStats.h"
#include "csrun/core/Expected.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace csrun {

struct VulkanWsiRequest {
  std::filesystem::path vertexSpv;
  std::filesystem::path fragmentSpv;
  std::optional<std::uint32_t> deviceIndex;
  int width = 800;
  int height = 600;
  /// Present for at least this many ms (also exits on Escape / window close).
  int presentMs = 3000;
  std::string title = "csrun — Vulkan WSI";
};

struct VulkanWsiResult {
  std::string deviceName;
  std::vector<std::string> steps;
  bool ok = false;
  std::uint32_t framesPresented = 0;
  double hostMs = 0.0;
  PipelineStats counters;
};

/// Create a real window, Vulkan surface, swapchain, draw, and present.
Expected<VulkanWsiResult> vulkanPresentGraphics(const VulkanWsiRequest &req);

} // namespace csrun

#endif
