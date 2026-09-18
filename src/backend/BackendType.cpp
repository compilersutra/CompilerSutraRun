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
  case BackendType::OpenGL:
    return "opengl";
  case BackendType::OpenGLES:
    return "opengles";
  case BackendType::OpenCL:
    return "opencl";
  case BackendType::CUDA:
    return "cuda";
  case BackendType::HIP:
    return "hip";
  case BackendType::D3D12:
    return "d3d12";
  case BackendType::Metal:
    return "metal";
  case BackendType::WebGPU:
    return "webgpu";
  case BackendType::CPU:
    return "cpu";
  }
  return "unknown";
}

std::optional<BackendType> parseBackendType(std::string_view text) {
  const auto t = lower(text);
  if (t == "vulkan" || t == "vk")
    return BackendType::Vulkan;
  if (t == "opengl" || t == "gl")
    return BackendType::OpenGL;
  if (t == "opengles" || t == "gles" || t == "es")
    return BackendType::OpenGLES;
  if (t == "opencl" || t == "cl")
    return BackendType::OpenCL;
  if (t == "cuda")
    return BackendType::CUDA;
  if (t == "hip" || t == "rocm")
    return BackendType::HIP;
  if (t == "d3d12" || t == "dx12" || t == "directx12" || t == "dxil")
    return BackendType::D3D12;
  if (t == "metal")
    return BackendType::Metal;
  if (t == "webgpu" || t == "wgpu")
    return BackendType::WebGPU;
  if (t == "cpu" || t == "host")
    return BackendType::CPU;
  return std::nullopt;
}

int defaultPriority(BackendType type) {
  // Lower is better. Prefer modern explicit APIs, then classic, then CPU baseline.
  switch (type) {
  case BackendType::Vulkan:
    return 10;
  case BackendType::CUDA:
    return 20;
  case BackendType::HIP:
    return 30;
  case BackendType::D3D12:
    return 40;
  case BackendType::Metal:
    return 50;
  case BackendType::OpenCL:
    return 60;
  case BackendType::OpenGL:
    return 70;
  case BackendType::OpenGLES:
    return 80;
  case BackendType::WebGPU:
    return 90;
  case BackendType::CPU:
    return 100;
  }
  return 1000;
}

} // namespace csrun
