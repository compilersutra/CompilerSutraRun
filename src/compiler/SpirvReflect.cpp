//===----------------------------------------------------------------------===//
//
// SpirvReflect.cpp
//
//===----------------------------------------------------------------------===//

#include "csrun/compiler/SpirvReflect.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace csrun {
namespace {

constexpr std::uint32_t kSpvMagic = 0x07230203u;

// Opcodes (subset)
constexpr std::uint16_t OpEntryPoint = 15;
constexpr std::uint16_t OpCapability = 17;
constexpr std::uint16_t OpTypeVoid = 19;
constexpr std::uint16_t OpTypeBool = 20;
constexpr std::uint16_t OpTypeInt = 21;
constexpr std::uint16_t OpTypeFloat = 22;
constexpr std::uint16_t OpTypeVector = 23;
constexpr std::uint16_t OpTypeMatrix = 24;
constexpr std::uint16_t OpTypeImage = 25;
constexpr std::uint16_t OpTypeSampler = 26;
constexpr std::uint16_t OpTypeSampledImage = 27;
constexpr std::uint16_t OpTypeArray = 28;
constexpr std::uint16_t OpTypeRuntimeArray = 29;
constexpr std::uint16_t OpTypeStruct = 30;
constexpr std::uint16_t OpTypePointer = 32;
constexpr std::uint16_t OpTypeForwardPointer = 39;
constexpr std::uint16_t OpConstant = 43;
constexpr std::uint16_t OpConstantComposite = 44;
constexpr std::uint16_t OpVariable = 59;
constexpr std::uint16_t OpDecorate = 71;
constexpr std::uint16_t OpMemberDecorate = 72;

// Decorations
constexpr std::uint32_t DecBlock = 2;
constexpr std::uint32_t DecBufferBlock = 3;
constexpr std::uint32_t DecBuiltIn = 11;
constexpr std::uint32_t DecLocation = 30;
constexpr std::uint32_t DecBinding = 33;
constexpr std::uint32_t DecDescriptorSet = 34;
constexpr std::uint32_t DecOffset = 35;
constexpr std::uint32_t DecArrayStride = 6;
constexpr std::uint32_t DecInputAttachmentIndex = 43;

// Storage classes
constexpr std::uint32_t SCUniformConstant = 0;
constexpr std::uint32_t SCInput = 1;
constexpr std::uint32_t SCUniform = 2;
constexpr std::uint32_t SCPushConstant = 9;
constexpr std::uint32_t SCStorageBuffer = 12;

// Capabilities
constexpr std::uint32_t CapClipDistance = 32;
constexpr std::uint32_t CapInputAttachment = 39;
constexpr std::uint32_t DimSubpassData = 6;

enum class TypeKind {
  Unknown,
  Void,
  Bool,
  Int,
  Float,
  Vector,
  Matrix,
  Image,
  Sampler,
  SampledImage,
  Array,
  RuntimeArray,
  Struct,
  Pointer,
};

struct TypeInfo {
  TypeKind kind = TypeKind::Unknown;
  std::uint32_t width = 0;       // int/float bits
  bool isSigned = true;
  std::uint32_t elemCount = 0;   // vector comps / matrix cols / array length
  std::uint32_t elemType = 0;    // id
  std::uint32_t storageClass = 0;
  std::uint32_t imageDim = 0;
  std::uint32_t imageSampled = 0; // 1=sampled, 2=storage
  std::vector<std::uint32_t> members; // struct member type ids
};

struct Decor {
  bool hasSet = false;
  bool hasBinding = false;
  bool hasLocation = false;
  bool hasBuiltIn = false;
  bool block = false;
  bool bufferBlock = false;
  bool hasInputAttachmentIndex = false;
  std::uint32_t set = 0;
  std::uint32_t binding = 0;
  std::uint32_t location = 0;
  std::uint32_t arrayStride = 0;
  std::uint32_t inputAttachmentIndex = 0;
};

struct MemberDecor {
  std::unordered_map<std::uint32_t, std::uint32_t> offset; // member index → offset
};

std::uint32_t typeSizeBytes(const std::unordered_map<std::uint32_t, TypeInfo> &types,
                            std::uint32_t typeId,
                            const std::unordered_map<std::uint32_t, MemberDecor> &memberDecors,
                            int depth = 0) {
  if (depth > 32)
    return 0;
  auto it = types.find(typeId);
  if (it == types.end())
    return 0;
  const TypeInfo &t = it->second;
  switch (t.kind) {
  case TypeKind::Bool:
  case TypeKind::Int:
  case TypeKind::Float:
    return (t.width + 7) / 8;
  case TypeKind::Vector: {
    const std::uint32_t es = typeSizeBytes(types, t.elemType, memberDecors, depth + 1);
    return es * t.elemCount;
  }
  case TypeKind::Matrix: {
    // columns of vectors
    const std::uint32_t es = typeSizeBytes(types, t.elemType, memberDecors, depth + 1);
    return es * t.elemCount;
  }
  case TypeKind::Array: {
    const std::uint32_t es = typeSizeBytes(types, t.elemType, memberDecors, depth + 1);
    std::uint32_t stride = es;
    // Prefer ArrayStride decoration on the array type id if present — handled via caller
    return stride * std::max<std::uint32_t>(1, t.elemCount);
  }
  case TypeKind::RuntimeArray:
    return typeSizeBytes(types, t.elemType, memberDecors, depth + 1);
  case TypeKind::Struct: {
    auto md = memberDecors.find(typeId);
    std::uint32_t maxEnd = 0;
    for (std::uint32_t i = 0; i < t.members.size(); ++i) {
      const std::uint32_t ms = typeSizeBytes(types, t.members[i], memberDecors, depth + 1);
      std::uint32_t off = 0;
      if (md != memberDecors.end()) {
        auto o = md->second.offset.find(i);
        if (o != md->second.offset.end())
          off = o->second;
        else
          off = maxEnd;
      } else {
        off = maxEnd;
      }
      maxEnd = std::max(maxEnd, off + ms);
    }
    return maxEnd;
  }
  case TypeKind::Pointer:
    return typeSizeBytes(types, t.elemType, memberDecors, depth + 1);
  default:
    return 0;
  }
}

/// Flatten every matrix in a uniform/storage block down to an absolute byte offset.
///
/// A block is rarely a flat run of mat4s: `gears` puts `model[3]` after a vec4, and
/// `texturearray` puts `Instance { mat4 model; float4 arrayIndex; } instance[8]` after
/// two mat4s. A flat, top-level-only view misses both, so smoke defaults would leave
/// the model matrix zero and collapse the geometry (`CLIP_P=0`).
///
/// Structs are walked recursively and array elements are expanded, so a caller can
/// simply write identity at every reported offset. Bounded by `maxMatrices` so a
/// runtime-sized array cannot make this unbounded.
void collectMatricesAt(const std::unordered_map<std::uint32_t, TypeInfo> &types,
                       std::uint32_t typeId, std::uint32_t baseOffset,
                       const std::unordered_map<std::uint32_t, MemberDecor> &memberDecors,
                       std::vector<SpirvBlockMember> &out, int depth,
                       std::size_t maxMatrices) {
  if (depth > 8 || out.size() >= maxMatrices)
    return;
  auto sit = types.find(typeId);
  if (sit == types.end())
    return;
  const TypeInfo &st = sit->second;
  auto md = memberDecors.find(typeId);
  if (md == memberDecors.end())
    return;
  for (std::uint32_t i = 0; i < st.members.size() && out.size() < maxMatrices; ++i) {
    auto o = md->second.offset.find(i);
    if (o == md->second.offset.end())
      continue;
    const std::uint32_t off = baseOffset + o->second;
    std::uint32_t shapeId = st.members[i];
    // Unwrap arrays to reach the element type; the member's full size is kept so the
    // caller can tell how many elements the array spans.
    std::uint32_t span = 1;
    for (int unwrap = 0; unwrap < 4; ++unwrap) {
      auto ait = types.find(shapeId);
      if (ait == types.end())
        break;
      if (ait->second.kind != TypeKind::Array && ait->second.kind != TypeKind::RuntimeArray)
        break;
      shapeId = ait->second.elemType;
      if (span == 1)
        span = std::max<std::uint32_t>(1, ait->second.elemCount);
    }
    auto mit = types.find(shapeId);
    if (mit == types.end())
      continue;
    if (mit->second.kind == TypeKind::Matrix) {
      SpirvBlockMember m;
      m.offset = off;
      m.columns = mit->second.elemCount;
      auto cit = types.find(mit->second.elemType);
      m.rows = (cit != types.end()) ? cit->second.elemCount : 0;
      m.isMatrix = true;
      m.sizeBytes = m.rows * m.columns * 4u * span;
      out.push_back(m);
      continue;
    }
    if (mit->second.kind == TypeKind::Struct) {
      const std::uint32_t elemSize = typeSizeBytes(types, shapeId, memberDecors);
      if (elemSize == 0)
        continue;
      for (std::uint32_t e = 0; e < span && out.size() < maxMatrices; ++e)
        collectMatricesAt(types, shapeId, off + e * elemSize, memberDecors, out, depth + 1,
                          maxMatrices);
    }
  }
}

bool collectBlockMembers(const std::unordered_map<std::uint32_t, TypeInfo> &types,
                         std::uint32_t blockTypeId,
                         const std::unordered_map<std::uint32_t, MemberDecor> &memberDecors,
                         std::vector<SpirvBlockMember> &out) {
  auto sit = types.find(blockTypeId);
  if (sit == types.end() || sit->second.kind != TypeKind::Struct)
    return false;
  if (memberDecors.find(blockTypeId) == memberDecors.end())
    return false;
  constexpr std::size_t kMaxMatrices = 256;
  const std::size_t before = out.size();
  collectMatricesAt(types, blockTypeId, 0, memberDecors, out, 0, kMaxMatrices);
  return out.size() > before;
}

TypeInfo unwrapPointer(const std::unordered_map<std::uint32_t, TypeInfo> &types, std::uint32_t id) {
  auto it = types.find(id);
  if (it == types.end())
    return {};
  if (it->second.kind == TypeKind::Pointer)
    return unwrapPointer(types, it->second.elemType);
  return it->second;
}

SpirvDescriptorKind classifyDescriptor(const TypeInfo &pointee, const Decor &d,
                                       std::uint32_t storage) {
  if (storage == SCStorageBuffer || d.bufferBlock)
    return SpirvDescriptorKind::StorageBuffer;
  if (storage == SCUniform || d.block)
    return SpirvDescriptorKind::UniformBuffer;
  if (pointee.kind == TypeKind::SampledImage)
    return SpirvDescriptorKind::CombinedImageSampler;
  if (pointee.kind == TypeKind::Sampler)
    return SpirvDescriptorKind::Sampler;
  if (pointee.kind == TypeKind::Image) {
    // `subpassInput` is an image with Dim=SubpassData; InputAttachmentIndex says which
    // render pass attachment it reads. The decoration is the authoritative signal (an
    // array of subpassInputs is decorated on the variable, not per element), but the
    // Dim alone is enough to avoid binding it as a sampled image.
    if (pointee.imageDim == DimSubpassData || d.hasInputAttachmentIndex)
      return SpirvDescriptorKind::InputAttachment;
    if (pointee.imageSampled == 2)
      return SpirvDescriptorKind::StorageImage;
    return SpirvDescriptorKind::SampledImage;
  }
  if (storage == SCUniformConstant) {
    if (pointee.kind == TypeKind::SampledImage)
      return SpirvDescriptorKind::CombinedImageSampler;
  }
  return SpirvDescriptorKind::Unknown;
}

} // namespace

std::string_view toString(SpirvDescriptorKind kind) {
  switch (kind) {
  case SpirvDescriptorKind::UniformBuffer:
    return "uniform_buffer";
  case SpirvDescriptorKind::StorageBuffer:
    return "storage_buffer";
  case SpirvDescriptorKind::CombinedImageSampler:
    return "combined_image_sampler";
  case SpirvDescriptorKind::SampledImage:
    return "sampled_image";
  case SpirvDescriptorKind::Sampler:
    return "sampler";
  case SpirvDescriptorKind::StorageImage:
    return "storage_image";
  case SpirvDescriptorKind::InputAttachment:
    return "input_attachment";
  default:
    return "unknown";
  }
}

Expected<SpirvModuleReflection> reflectSpirvModule(const std::vector<std::uint32_t> &words,
                                                   SpirvReflectStage stage) {
  if (words.size() < 5 || words[0] != kSpvMagic) {
    return Error::make(ErrorCode::CompilationFailed, "SPIR-V reflect: bad magic / too short");
  }
  const bool isVertex = stage == SpirvReflectStage::Vertex;
  const bool isFragment = stage == SpirvReflectStage::Fragment;
  const bool isGeometry = stage == SpirvReflectStage::Geometry;
  const bool isTessControl = stage == SpirvReflectStage::TessControl;
  const bool isTessEval = stage == SpirvReflectStage::TessEvaluation;
  const bool isMesh = stage == SpirvReflectStage::Mesh;
  const bool isTask = stage == SpirvReflectStage::Task;

  SpirvModuleReflection out;
  std::unordered_map<std::uint32_t, TypeInfo> types;
  std::unordered_map<std::uint32_t, Decor> decors;
  std::unordered_map<std::uint32_t, MemberDecor> memberDecors;
  std::unordered_map<std::uint32_t, std::uint32_t> constInt; // id → value

  for (std::size_t i = 5; i < words.size();) {
    const std::uint32_t first = words[i];
    const std::uint16_t opcode = static_cast<std::uint16_t>(first & 0xffffu);
    const std::uint16_t wc = static_cast<std::uint16_t>(first >> 16);
    if (wc == 0 || i + wc > words.size())
      break;
    const std::uint32_t *op = &words[i];

    switch (opcode) {
    case OpCapability:
      if (wc >= 2) {
        if (op[1] == CapClipDistance)
          out.usesClipDistance = true;
        if (op[1] == CapInputAttachment)
          out.usesInputAttachment = true;
      }
      break;
    case OpEntryPoint:
      break;
    case OpTypeVoid:
      if (wc >= 2)
        types[op[1]] = TypeInfo{TypeKind::Void, 0, true, 0, 0, 0, 0, 0, {}};
      break;
    case OpTypeBool:
      if (wc >= 2)
        types[op[1]] = TypeInfo{TypeKind::Bool, 32, true, 0, 0, 0, 0, 0, {}};
      break;
    case OpTypeInt:
      if (wc >= 4) {
        TypeInfo t{TypeKind::Int, 0, true, 0, 0, 0, 0, 0, {}};
        t.width = op[2];
        t.isSigned = op[3] != 0;
        types[op[1]] = t;
      }
      break;
    case OpTypeFloat:
      if (wc >= 3) {
        TypeInfo t{TypeKind::Float, 0, true, 0, 0, 0, 0, 0, {}};
        t.width = op[2];
        types[op[1]] = t;
      }
      break;
    case OpTypeVector:
      if (wc >= 4) {
        TypeInfo t{TypeKind::Vector, 0, true, 0, 0, 0, 0, 0, {}};
        t.elemType = op[2];
        t.elemCount = op[3];
        types[op[1]] = t;
      }
      break;
    case OpTypeMatrix:
      if (wc >= 4) {
        TypeInfo t{TypeKind::Matrix, 0, true, 0, 0, 0, 0, 0, {}};
        t.elemType = op[2];
        t.elemCount = op[3];
        types[op[1]] = t;
      }
      break;
    case OpTypeImage:
      if (wc >= 2) {
        TypeInfo ti{TypeKind::Image, 0, true, 0, 0, 0, 0, 0, {}};
        if (wc >= 4)
          ti.imageDim = op[3];
        if (wc >= 8)
          ti.imageSampled = op[7];
        if (ti.imageDim == DimSubpassData)
          out.usesInputAttachment = true;
        types[op[1]] = ti;
      }
      break;
    case OpTypeSampler:
      if (wc >= 2)
        types[op[1]] = TypeInfo{TypeKind::Sampler, 0, true, 0, 0, 0, 0, 0, {}};
      break;
    case OpTypeSampledImage:
      if (wc >= 2)
        types[op[1]] = TypeInfo{TypeKind::SampledImage, 0, true, 0, 0, 0, 0, 0, {}};
      break;
    case OpTypeArray:
      if (wc >= 4) {
        TypeInfo t{TypeKind::Array, 0, true, 0, 0, 0, 0, 0, {}};
        t.elemType = op[2];
        auto cit = constInt.find(op[3]);
        t.elemCount = cit != constInt.end() ? cit->second : 1;
        types[op[1]] = t;
      }
      break;
    case OpTypeRuntimeArray:
      if (wc >= 3) {
        TypeInfo t{TypeKind::RuntimeArray, 0, true, 0, 0, 0, 0, 0, {}};
        t.elemType = op[2];
        types[op[1]] = t;
      }
      break;
    case OpTypeStruct:
      if (wc >= 2) {
        TypeInfo t{TypeKind::Struct, 0, true, 0, 0, 0, 0, 0, {}};
        for (std::uint16_t k = 2; k < wc; ++k)
          t.members.push_back(op[k]);
        types[op[1]] = t;
      }
      break;
    case OpTypePointer:
      if (wc >= 4) {
        TypeInfo t{TypeKind::Pointer, 0, true, 0, 0, 0, 0, 0, {}};
        t.storageClass = op[2];
        t.elemType = op[3];
        types[op[1]] = t;
      }
      break;
    case OpTypeForwardPointer:
      break;
    case OpConstant:
      if (wc >= 4) {
        // type id, result id, value...
        constInt[op[2]] = op[3];
      }
      break;
    case OpConstantComposite:
      break;
    case OpDecorate:
      if (wc >= 3) {
        Decor &d = decors[op[1]];
        const std::uint32_t dec = op[2];
        if (dec == DecBlock)
          d.block = true;
        else if (dec == DecBufferBlock)
          d.bufferBlock = true;
        else if (dec == DecBuiltIn)
          d.hasBuiltIn = true;
        else if (dec == DecLocation && wc >= 4) {
          d.hasLocation = true;
          d.location = op[3];
        } else if (dec == DecBinding && wc >= 4) {
          d.hasBinding = true;
          d.binding = op[3];
        } else if (dec == DecDescriptorSet && wc >= 4) {
          d.hasSet = true;
          d.set = op[3];
        } else if (dec == DecArrayStride && wc >= 4) {
          d.arrayStride = op[3];
        } else if (dec == DecInputAttachmentIndex && wc >= 4) {
          d.hasInputAttachmentIndex = true;
          d.inputAttachmentIndex = op[3];
        }
      }
      break;
    case OpMemberDecorate:
      if (wc >= 4) {
        const std::uint32_t typeId = op[1];
        const std::uint32_t member = op[2];
        const std::uint32_t dec = op[3];
        if (dec == DecOffset && wc >= 5)
          memberDecors[typeId].offset[member] = op[4];
      }
      break;
    case OpVariable:
      if (wc >= 4) {
        const std::uint32_t resultType = op[1];
        const std::uint32_t resultId = op[2];
        const std::uint32_t storage = op[3];
        auto tit = types.find(resultType);
        if (tit == types.end() || tit->second.kind != TypeKind::Pointer)
          break;
        const TypeInfo &ptr = tit->second;
        const Decor &d = decors[resultId];
        const TypeInfo pointee = unwrapPointer(types, resultType);

        if (storage == SCInput && isVertex && d.hasLocation && !d.hasBuiltIn) {
          SpirvVertexAttribute attr;
          attr.location = d.location;
          TypeInfo base = pointee;
          if (base.kind == TypeKind::Vector) {
            attr.components = base.elemCount;
            auto et = types.find(base.elemType);
            if (et != types.end() && et->second.kind == TypeKind::Int) {
              attr.type = et->second.isSigned ? SpirvVertexCompType::Int : SpirvVertexCompType::Uint;
            } else {
              attr.type = SpirvVertexCompType::Float;
            }
          } else if (base.kind == TypeKind::Int) {
            attr.components = 1;
            attr.type = base.isSigned ? SpirvVertexCompType::Int : SpirvVertexCompType::Uint;
          } else {
            attr.components = 1;
            attr.type = SpirvVertexCompType::Float;
          }
          out.vertexInputs.push_back(attr);
        }

        if (storage == SCPushConstant) {
          SpirvPushConstantRange pc;
          pc.size = typeSizeBytes(types, ptr.elemType, memberDecors);
          if (pc.size == 0)
            pc.size = 128;
          pc.fromVertex = isVertex;
          pc.fromFragment = isFragment;
          pc.fromGeometry = isGeometry;
          pc.fromTessControl = isTessControl;
          pc.fromTessEvaluation = isTessEval;
          pc.fromMesh = isMesh;
          pc.fromTask = isTask;
          out.pushConstants.push_back(pc);
        }

        const bool isDesc =
            (storage == SCUniform || storage == SCUniformConstant || storage == SCStorageBuffer) &&
            d.hasBinding;
        if (isDesc) {
          SpirvDescriptorBinding b;
          b.set = d.hasSet ? d.set : 0;
          b.binding = d.binding;
          TypeInfo descPointee = pointee;
          b.count = 1;
          if (pointee.kind == TypeKind::Array) {
            b.count = std::max<std::uint32_t>(1, pointee.elemCount);
            auto et = types.find(pointee.elemType);
            if (et != types.end())
              descPointee = et->second;
          } else if (pointee.kind == TypeKind::RuntimeArray) {
            // Smoke: fixed bind count for unbounded arrays (descriptor indexing).
            constexpr std::uint32_t kSmokeRuntimeArrayCount = 8;
            b.count = kSmokeRuntimeArrayCount;
            out.usesRuntimeDescriptorArray = true;
            auto et = types.find(pointee.elemType);
            if (et != types.end())
              descPointee = et->second;
          }
          b.kind = classifyDescriptor(descPointee, d, storage);
          if (b.kind == SpirvDescriptorKind::InputAttachment)
            b.inputAttachmentIndex = d.inputAttachmentIndex;
          if (b.kind == SpirvDescriptorKind::UniformBuffer ||
              b.kind == SpirvDescriptorKind::StorageBuffer) {
            b.sizeBytes = typeSizeBytes(types, ptr.elemType, memberDecors);
            if (b.sizeBytes == 0)
              b.sizeBytes = 256;
            collectBlockMembers(types, ptr.elemType, memberDecors, b.members);
          }
          b.fromVertex = isVertex;
          b.fromFragment = isFragment;
          b.fromGeometry = isGeometry;
          b.fromTessControl = isTessControl;
          b.fromTessEvaluation = isTessEval;
          b.fromMesh = isMesh;
          b.fromTask = isTask;
          out.descriptors.push_back(b);
        }
      }
      break;
    default:
      break;
    }
    i += wc;
  }

  std::sort(out.vertexInputs.begin(), out.vertexInputs.end(),
            [](const SpirvVertexAttribute &a, const SpirvVertexAttribute &b) {
              return a.location < b.location;
            });
  return out;
}

SpirvModuleReflection mergeSpirvReflections(const SpirvModuleReflection &vert,
                                            const SpirvModuleReflection &frag,
                                            const SpirvExtraStageReflections &extra) {
  SpirvModuleReflection out;
  auto orFlags = [](SpirvModuleReflection &o, const SpirvModuleReflection &m) {
    o.usesClipDistance = o.usesClipDistance || m.usesClipDistance;
    o.usesInputAttachment = o.usesInputAttachment || m.usesInputAttachment;
    o.usesRuntimeDescriptorArray = o.usesRuntimeDescriptorArray || m.usesRuntimeDescriptorArray;
    o.usesShaderClock = o.usesShaderClock || m.usesShaderClock;
  };
  out.usesClipDistance = vert.usesClipDistance || frag.usesClipDistance;
  out.usesInputAttachment = vert.usesInputAttachment || frag.usesInputAttachment;
  out.usesRuntimeDescriptorArray =
      vert.usesRuntimeDescriptorArray || frag.usesRuntimeDescriptorArray;
  out.usesShaderClock = vert.usesShaderClock || frag.usesShaderClock;
  if (extra.geometry)
    orFlags(out, *extra.geometry);
  if (extra.tessControl)
    orFlags(out, *extra.tessControl);
  if (extra.tessEvaluation)
    orFlags(out, *extra.tessEvaluation);
  if (extra.mesh)
    orFlags(out, *extra.mesh);
  if (extra.task)
    orFlags(out, *extra.task);

  out.vertexInputs = vert.vertexInputs;
  out.pushConstants = vert.pushConstants;

  struct StageMark {
    bool frag = false;
    bool geom = false;
    bool tesc = false;
    bool tese = false;
    bool mesh = false;
    bool task = false;
  };
  auto applyPcMark = [](SpirvPushConstantRange &e, StageMark m) {
    if (m.frag)
      e.fromFragment = true;
    if (m.geom)
      e.fromGeometry = true;
    if (m.tesc)
      e.fromTessControl = true;
    if (m.tese)
      e.fromTessEvaluation = true;
    if (m.mesh)
      e.fromMesh = true;
    if (m.task)
      e.fromTask = true;
  };
  auto mergePc = [&](const SpirvPushConstantRange &pc, StageMark m) {
    for (auto &e : out.pushConstants) {
      if (e.offset == pc.offset) {
        e.size = std::max(e.size, pc.size);
        applyPcMark(e, m);
        return;
      }
    }
    SpirvPushConstantRange n = pc;
    applyPcMark(n, m);
    out.pushConstants.push_back(n);
  };
  for (const auto &pc : frag.pushConstants)
    mergePc(pc, StageMark{true, false, false, false, false, false});
  if (extra.geometry) {
    for (const auto &pc : extra.geometry->pushConstants)
      mergePc(pc, StageMark{false, true, false, false, false, false});
  }
  if (extra.tessControl) {
    for (const auto &pc : extra.tessControl->pushConstants)
      mergePc(pc, StageMark{false, false, true, false, false, false});
  }
  if (extra.tessEvaluation) {
    for (const auto &pc : extra.tessEvaluation->pushConstants)
      mergePc(pc, StageMark{false, false, false, true, false, false});
  }
  if (extra.mesh) {
    for (const auto &pc : extra.mesh->pushConstants)
      mergePc(pc, StageMark{false, false, false, false, true, false});
  }
  if (extra.task) {
    for (const auto &pc : extra.task->pushConstants)
      mergePc(pc, StageMark{false, false, false, false, false, true});
  }

  auto key = [](const SpirvDescriptorBinding &b) {
    return (static_cast<std::uint64_t>(b.set) << 32) | b.binding;
  };
  std::unordered_map<std::uint64_t, SpirvDescriptorBinding> map;
  for (const auto &b : vert.descriptors)
    map[key(b)] = b;

  auto applyDescMark = [](SpirvDescriptorBinding &e, StageMark m) {
    if (m.frag)
      e.fromFragment = true;
    if (m.geom)
      e.fromGeometry = true;
    if (m.tesc)
      e.fromTessControl = true;
    if (m.tese)
      e.fromTessEvaluation = true;
    if (m.mesh)
      e.fromMesh = true;
    if (m.task)
      e.fromTask = true;
  };
  auto mergeDesc = [&](const SpirvDescriptorBinding &b, StageMark m) {
    auto it = map.find(key(b));
    if (it == map.end()) {
      SpirvDescriptorBinding n = b;
      applyDescMark(n, m);
      map[key(b)] = n;
    } else {
      applyDescMark(it->second, m);
      it->second.sizeBytes = std::max(it->second.sizeBytes, b.sizeBytes);
      it->second.count = std::max(it->second.count, b.count);
      if (it->second.kind == SpirvDescriptorKind::Unknown)
        it->second.kind = b.kind;
    }
  };
  for (const auto &b : frag.descriptors)
    mergeDesc(b, StageMark{true, false, false, false, false, false});
  if (extra.geometry) {
    for (const auto &b : extra.geometry->descriptors)
      mergeDesc(b, StageMark{false, true, false, false, false, false});
  }
  if (extra.tessControl) {
    for (const auto &b : extra.tessControl->descriptors)
      mergeDesc(b, StageMark{false, false, true, false, false, false});
  }
  if (extra.tessEvaluation) {
    for (const auto &b : extra.tessEvaluation->descriptors)
      mergeDesc(b, StageMark{false, false, false, true, false, false});
  }
  if (extra.mesh) {
    for (const auto &b : extra.mesh->descriptors)
      mergeDesc(b, StageMark{false, false, false, false, true, false});
  }
  if (extra.task) {
    for (const auto &b : extra.task->descriptors)
      mergeDesc(b, StageMark{false, false, false, false, false, true});
  }

  out.descriptors.reserve(map.size());
  for (auto &kv : map)
    out.descriptors.push_back(kv.second);
  std::sort(out.descriptors.begin(), out.descriptors.end(),
            [](const SpirvDescriptorBinding &a, const SpirvDescriptorBinding &b) {
              if (a.set != b.set)
                return a.set < b.set;
              return a.binding < b.binding;
            });
  return out;
}


} // namespace csrun
