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

struct SpirvDescriptorBinding {
  std::uint32_t set = 0;
  std::uint32_t binding = 0;
  SpirvDescriptorKind kind = SpirvDescriptorKind::Unknown;
  std::uint32_t count = 1;
  /// Best-effort byte size for UBO/SSBO (0 → caller picks default).
  std::uint32_t sizeBytes = 0;
  bool fromVertex = false;
  bool fromFragment = false;
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

/// Reflect one SPIR-V module. `isVertex` marks stage for descriptor stage flags merge.
Expected<SpirvModuleReflection> reflectSpirvModule(const std::vector<std::uint32_t> &words,
                                                   bool isVertex);

/// Merge vertex + fragment reflections (descriptors by set/binding, attrs from vertex).
SpirvModuleReflection mergeSpirvReflections(const SpirvModuleReflection &vert,
                                            const SpirvModuleReflection &frag);

std::string_view toString(SpirvDescriptorKind kind);

} // namespace csrun

#endif
