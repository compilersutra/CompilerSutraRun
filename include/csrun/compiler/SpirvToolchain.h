//===----------------------------------------------------------------------===//
//
// SpirvToolchain.h
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_COMPILER_SPIRVTOOLCHAIN_H
#define CSRUN_COMPILER_SPIRVTOOLCHAIN_H

#include "csrun/core/Expected.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace csrun {

enum class ShaderStage {
  Unknown,
  Compute,
  Vertex,
  Fragment,
  TessControl,
  TessEvaluation,
  Geometry,
  RayGen,
  AnyHit,
  ClosestHit,
  Miss,
  Intersection,
  Callable,
  Mesh,
  Task,
};

struct SpirvCompileRequest {
  std::filesystem::path input;
  std::filesystem::path output;
  bool validate = true;
  bool keepOutput = true;
};

struct SpirvCompileResult {
  ShaderStage stage = ShaderStage::Unknown;
  std::filesystem::path spvPath;
  std::vector<uint32_t> words;
  std::string compiler;
};

std::string_view toString(ShaderStage stage);
ShaderStage shaderStageFromPath(const std::filesystem::path &path);
std::string_view glslangStageToken(ShaderStage stage);

class SpirvToolchain {
public:
  static Expected<SpirvCompileResult> compile(const SpirvCompileRequest &req);
  static Expected<std::vector<uint32_t>> loadSpvFile(const std::filesystem::path &spvPath);
  static Expected<void> validateSpvFile(const std::filesystem::path &spvPath);
};

} // namespace csrun

#endif
