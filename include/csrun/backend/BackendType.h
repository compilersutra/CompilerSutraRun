//===----------------------------------------------------------------------===//
// BackendType.h
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKEND_BACKENDTYPE_H
#define CSRUN_BACKEND_BACKENDTYPE_H

#include <optional>
#include <string_view>

namespace csrun {

/// Every execution ecosystem the platform intends to orchestrate.
/// Unavailable backends must report UNAVAILABLE — never fake success.
enum class BackendType {
  Vulkan,   // SPIR-V
  OpenGL,   // classic Linux/desktop GL (GLSL → driver)
  OpenGLES, // OpenGL ES
  OpenCL,   // portable compute
  CUDA,     // PTX / cubin
  HIP,      // ROCm
  D3D12,    // DXIL (Windows)
  Metal,    // Apple
  WebGPU,   // Dawn / wgpu
  CPU,      // host reference / native CPU
};

std::string_view toString(BackendType type);
std::optional<BackendType> parseBackendType(std::string_view text);

/// Stable default ranking when capability scores tie (lower = preferred).
int defaultPriority(BackendType type);

} // namespace csrun

#endif
