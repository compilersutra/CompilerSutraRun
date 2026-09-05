//===----------------------------------------------------------------------===//
// BackendType.h
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKEND_BACKENDTYPE_H
#define CSRUN_BACKEND_BACKENDTYPE_H

#include <optional>
#include <string_view>

namespace csrun {

enum class BackendType {
  Vulkan,
  CUDA,
  D3D12,
  HIP,
  Metal,
  WebGPU,
  OpenCL,
};

std::string_view toString(BackendType type);
std::optional<BackendType> parseBackendType(std::string_view text);

/// Stable default ranking when capability scores tie.
int defaultPriority(BackendType type);

} // namespace csrun

#endif
