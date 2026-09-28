//===----------------------------------------------------------------------===//
//
// SpirvReflect.h — lightweight SPIR-V descriptor / vertex-input reflection
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_COMPILER_SPIRVREFLECT_H
#define CSRUN_COMPILER_SPIRVREFLECT_H

#include "csrun/core/Expected.h"

#include <cstdint>
#include <string>
#include <vector>

namespace csrun {

enum class SpirvDescriptorKind {
  UniformBuffer,
  StorageBuffer,
  CombinedImageSampler,
  SampledImage,
  Sampler,
  StorageImage,
  Unknown,
};

enum class SpirvVertexCompType {
  Float,
  Int,
  Uint,
};

enum class SpirvReflectStage {
  Vertex,
  Fragment,
  Geometry,
  TessControl,
  TessEvaluation,
  Mesh,
  Task,
};

/// A matrix found inside a uniform/storage block, at an absolute byte offset.
struct SpirvBlockMember {
  std::uint32_t offset = 0;
  /// Total bytes the matrix occupies, including every element when it is an array
  /// (`mat4 model[3]` → 192). Zero-padded array tails are not counted.
  std::uint32_t sizeBytes = 0;
  bool isMatrix = true;
  std::uint32_t columns = 0; // mat4 → 4
  std::uint32_t rows = 0;    // mat4 → 4
};

struct SpirvDescriptorBinding {
  std::uint32_t set = 0;
  std::uint32_t binding = 0;
  SpirvDescriptorKind kind = SpirvDescriptorKind::Unknown;
  std::uint32_t count = 1;
  /// Best-effort byte size for UBO/SSBO (0 → caller picks default).
  std::uint32_t sizeBytes = 0;
  /// Matrices reachable from the block, flattened to absolute byte offsets. Structs are
  /// walked recursively and array elements are expanded, so `Instance{ mat4 model; ... }
  /// instance[8]` yields eight entries. Empty when the module did not decorate member
  /// offsets, or the block declares no matrices — callers must keep a fallback then.
  std::vector<SpirvBlockMember> members;
  bool fromVertex = false;
  bool fromFragment = false;
  bool fromGeometry = false;
  bool fromTessControl = false;
  bool fromTessEvaluation = false;
  bool fromMesh = false;
  bool fromTask = false;
};

struct SpirvVertexAttribute {
  std::uint32_t location = 0;
  std::uint32_t components = 1; // 1..4
  SpirvVertexCompType type = SpirvVertexCompType::Float;
};

struct SpirvPushConstantRange {
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
  bool fromVertex = false;
  bool fromFragment = false;
  bool fromGeometry = false;
  bool fromTessControl = false;
  bool fromTessEvaluation = false;
  bool fromMesh = false;
  bool fromTask = false;
};

struct SpirvModuleReflection {
  std::vector<SpirvDescriptorBinding> descriptors;
  std::vector<SpirvVertexAttribute> vertexInputs;
  std::vector<SpirvPushConstantRange> pushConstants;
  bool usesClipDistance = false;
  bool usesShaderClock = false;
  /// True if module uses subpass/input attachments (Dim=SubpassData).
  bool usesInputAttachment = false;
  /// True if any descriptor uses OpTypeRuntimeArray (needs indexing features).
  bool usesRuntimeDescriptorArray = false;
};

struct SpirvExtraStageReflections {
  const SpirvModuleReflection *geometry = nullptr;
  const SpirvModuleReflection *tessControl = nullptr;
  const SpirvModuleReflection *tessEvaluation = nullptr;
  const SpirvModuleReflection *mesh = nullptr;
  const SpirvModuleReflection *task = nullptr;
};

/// Reflect one SPIR-V module; `stage` selects descriptor stage-flag bits.
Expected<SpirvModuleReflection> reflectSpirvModule(const std::vector<std::uint32_t> &words,
                                                   SpirvReflectStage stage);

/// Merge vertex + fragment (+ optional geometry / tess) reflections.
SpirvModuleReflection mergeSpirvReflections(const SpirvModuleReflection &vert,
                                            const SpirvModuleReflection &frag,
                                            const SpirvExtraStageReflections &extra = {});

std::string_view toString(SpirvDescriptorKind kind);

} // namespace csrun

#endif
