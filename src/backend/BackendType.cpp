#include "csrun/backend/BackendType.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace csrun {
namespace {

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

} // namespace

std::string_view toString(BackendType type) {
  switch (type) {
  case BackendType::Vulkan:
    return "vulkan";
  case BackendType::CUDA:
    return "cuda";
  case BackendType::D3D12:
    return "d3d12";
  case BackendType::HIP:
    return "hip";
  case BackendType::Metal:
    return "metal";
  case BackendType::WebGPU:
    return "webgpu";
  case BackendType::OpenCL:
    return "opencl";
  }
  return "unknown";
}

std::optional<BackendType> parseBackendType(std::string_view text) {
  const auto t = lower(text);
  if (t == "vulkan" || t == "vk")
    return BackendType::Vulkan;
  if (t == "cuda")
    return BackendType::CUDA;
  if (t == "d3d12" || t == "dx12" || t == "directx12")
    return BackendType::D3D12;
  if (t == "hip" || t == "rocm")
    return BackendType::HIP;
  if (t == "metal")
    return BackendType::Metal;
  if (t == "webgpu" || t == "wgpu")
    return BackendType::WebGPU;
  if (t == "opencl" || t == "cl")
    return BackendType::OpenCL;
  return std::nullopt;
}

int defaultPriority(BackendType type) {
  // Lower is better. Stable deterministic order for ties.
  switch (type) {
  case BackendType::Vulkan:
    return 10;
  case BackendType::CUDA:
    return 20;
  case BackendType::HIP:
    return 30;
  case BackendType::D3D12:
    return 40;
  case BackendType::OpenCL:
    return 50;
  case BackendType::Metal:
    return 60;
  case BackendType::WebGPU:
    return 70;
  }
  return 1000;
}

} // namespace csrun
