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
  ShaderStage stageOverride = ShaderStage::Unknown;
};

struct SpirvCompileResult {
  ShaderStage stage = ShaderStage::Unknown;
  std::filesystem::path spvPath;
  std::vector<uint32_t> words;
  std::string compiler;
};

struct SpirvModuleInfo {
  std::filesystem::path path;
  std::uint32_t version = 0;
  std::uint32_t generator = 0;
  std::uint32_t bound = 0;
  std::size_t wordCount = 0;
  std::string summary;
};

std::string_view toString(ShaderStage stage);
ShaderStage shaderStageFromPath(const std::filesystem::path &path);
ShaderStage parseShaderStage(std::string_view text);
std::string_view glslangStageToken(ShaderStage stage);

class SpirvToolchain {
public:
  static Expected<SpirvCompileResult> compile(const SpirvCompileRequest &req);
  static Expected<std::vector<uint32_t>> loadSpvFile(const std::filesystem::path &spvPath);
  static Expected<void> validateSpvFile(const std::filesystem::path &spvPath);
  static Expected<SpirvModuleInfo> inspectSpvFile(const std::filesystem::path &spvPath);
  /// Optional: run spirv-dis if installed; empty string if unavailable.
  static Expected<std::string> disassembleSpvFile(const std::filesystem::path &spvPath,
                                                  int maxLines = 40);
};

} // namespace csrun

#endif
