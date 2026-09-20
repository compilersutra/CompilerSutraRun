//===----------------------------------------------------------------------===//
//
// VulkanSpirvRunner.h
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKENDS_VULKAN_VULKANSPIRVRUNNER_H
#define CSRUN_BACKENDS_VULKAN_VULKANSPIRVRUNNER_H

#include "csrun/benchmark/Timing.h"
#include "csrun/benchmark/PipelineStats.h"
#include "csrun/benchmark/VendorCountersMode.h"
#include "csrun/compiler/SpirvToolchain.h"
#include "csrun/core/Expected.h"
#include "csrun/device/DeviceInfo.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace csrun {

struct VulkanSmokeRequest {
  std::filesystem::path computeSpv;
  std::filesystem::path vertexSpv;
  std::filesystem::path fragmentSpv;
  /// nullopt = auto-pick (prefer perf_query / RADV); set to force a physical index.
  std::optional<std::uint32_t> deviceIndex;
  /// When true (graphics): real Vulkan WSI present (Xlib surface + swapchain).
  /// When false: headless offscreen draw + readback (CI / SSH / csperf).
  bool show = false;
  int showMs = 3000;
  int windowWidth = 800;
  int windowHeight = 600;
  int warmup = 0;
  int iterations = 1;
  /// Vendor HW counter selection (default: single-pass).
  VendorCountersMode vendorCounters = VendorCountersMode::PreferSinglePass;
  /// Offscreen framebuffer (graphics).
  std::uint32_t width = 64;
  std::uint32_t height = 64;
  /// Compute size from workload.dimensions (count or M/N/K for matmul).
  std::uint64_t dimCount = 0;
  std::uint64_t dimM = 0;
  std::uint64_t dimN = 0;
  std::uint64_t dimK = 0;
  bool validate = false;
  double absTol = 1e-5;
};

struct VulkanSmokeResult {
  std::string deviceName;
  std::string mode;
  std::vector<std::string> steps;
  bool ok = false;
  bool executed = false;
  double hostMs = 0.0;
  std::vector<double> samplesMs;
  TimingSource timingSource = TimingSource::Unknown;
  PipelineStats counters;
  bool pixelChecked = false;
  bool computeChecked = false;
  std::uint8_t centerR = 0;
  std::uint8_t centerG = 0;
  std::uint8_t centerB = 0;
  std::uint8_t centerA = 0;
  std::uint32_t framesPresented = 0;
};

/// Execute SPIR-V: compute dispatch, or graphics (WSI present / headless offscreen).
Expected<VulkanSmokeResult> vulkanSmokeSpirv(const VulkanSmokeRequest &req);

} // namespace csrun

#endif
