//===----------------------------------------------------------------------===//
// Workload.h - Backend-independent workload model
//===----------------------------------------------------------------------===//

#ifndef CSRUN_WORKLOAD_WORKLOAD_H
#define CSRUN_WORKLOAD_WORKLOAD_H

#include "csrun/backend/BackendType.h"
#include "csrun/core/Types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace csrun {

struct ExecutionConfig {
  int warmup = 0;
  int iterations = 1;
  /// For graphics SPIR-V: open a window so the draw is visible.
  bool show = true;
  int showMs = 3000;
  /// Offscreen framebuffer size (also used as default WSI size when set).
  std::uint32_t width = 64;
  std::uint32_t height = 64;
};

struct ValidationConfig {
  bool enabled = true;
  double absoluteTolerance = 1e-5;
  double relativeTolerance = 1e-5;
};

/// Handwritten shader path (relative to YAML unless absolute).
struct ShaderRef {
  std::filesystem::path path;
  /// Optional stage override: vert|frag|comp|... Empty → detect from extension.
  std::string stage;
};

struct Workload {
  std::string name;
  std::string operation;
  Dimensions dimensions;
  DataType inputDtype = DataType::FP32;
  DataType outputDtype = DataType::FP32;
  ExecutionConfig execution;
  ValidationConfig validation;
  std::optional<BackendType> preferredBackend;
  std::string sourcePath;

  /// Handwritten sources for operation: spirv | shader | shader_pipeline.
  std::vector<ShaderRef> shaders;
  std::filesystem::path spvOutputDir;
};

inline bool isSpirvWorkload(const Workload &w) {
  return w.operation == "spirv" || w.operation == "shader" ||
         w.operation == "shader_pipeline" || w.operation == "vulkan_shaders";
}

} // namespace csrun

#endif
