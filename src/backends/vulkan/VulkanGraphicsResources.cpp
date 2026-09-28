//===----------------------------------------------------------------------===//
//
// VulkanGraphicsResources.cpp
//
//===----------------------------------------------------------------------===//

#include "csrun/backends/vulkan/VulkanGraphicsResources.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace csrun {
namespace {

Error vkErr(const std::string &msg, VkResult r) {
  Error e = Error::make(ErrorCode::DriverError, msg + " (VkResult=" + std::to_string(int(r)) + ")");
  e.backend = "vulkan";
  return e;
}

uint32_t findMem(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
      return i;
  }
  return std::numeric_limits<uint32_t>::max();
}

Expected<void> createHostBuffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                                VkBufferUsageFlags usage, VkBuffer *outBuf, VkDeviceMemory *outMem,
                                void **mapped) {
  VkBufferCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  ci.size = size;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult r = vkCreateBuffer(device, &ci, nullptr, outBuf);
  if (r != VK_SUCCESS)
    return vkErr("vkCreateBuffer failed", r);
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device, *outBuf, &req);
  const uint32_t mt = findMem(physical, req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mt == std::numeric_limits<uint32_t>::max()) {
    vkDestroyBuffer(device, *outBuf, nullptr);
    *outBuf = VK_NULL_HANDLE;
    return Error::make(ErrorCode::ResourceAllocationFailed, "no host-visible memory");
  }
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = mt;
  r = vkAllocateMemory(device, &ai, nullptr, outMem);
  if (r != VK_SUCCESS) {
    vkDestroyBuffer(device, *outBuf, nullptr);
    *outBuf = VK_NULL_HANDLE;
    return vkErr("vkAllocateMemory failed", r);
  }
  vkBindBufferMemory(device, *outBuf, *outMem, 0);
  if (mapped) {
    r = vkMapMemory(device, *outMem, 0, size, 0, mapped);
    if (r != VK_SUCCESS) {
      vkFreeMemory(device, *outMem, nullptr);
      vkDestroyBuffer(device, *outBuf, nullptr);
      *outBuf = VK_NULL_HANDLE;
      *outMem = VK_NULL_HANDLE;
      return vkErr("vkMapMemory failed", r);
    }
  }
  return {};
}

/// Write an identity matrix for an `rows`-major column matrix of `columns` columns
/// starting at `off`. Column c occupies floats [off + c*rows, off + c*rows + rows).
void writeIdentityMatrix(void *base, std::uint32_t off, std::uint32_t rows,
                         std::uint32_t columns) {
  if (rows != columns)
    return; // non-square: no diagonal identity, leave the zeros in place
  auto *f = reinterpret_cast<float *>(static_cast<std::uint8_t *>(base) + off);
  for (std::uint32_t c = 0; c < columns; ++c)
    f[c * rows + c] = 1.0f;
}

/// Fill a uniform/storage block with smoke defaults.
///
/// When the block's matrix layout was reflected, identity is written at each reported
/// matrix offset. The older "identity at every 64-byte boundary" heuristic is wrong for
/// any block that does not begin with a whole number of mat4s: `gears` declares
/// `projection; view; lightpos; model[3]`, so `model[0]` begins at byte 144 and the
/// heuristic never touched it, leaving the model matrix zero and collapsing the triangle
/// (`CLIP_P=0`). Without reflected layout we keep the old behaviour, which is correct for
/// the common mat4-first blocks.
void fillIdentityUbo(void *ptr, std::uint32_t sizeBytes,
                     const std::vector<SpirvBlockMember> *matrices = nullptr) {
  std::memset(ptr, 0, sizeBytes);
  if (matrices != nullptr && !matrices->empty()) {
    for (const SpirvBlockMember &m : *matrices) {
      if (m.offset >= sizeBytes)
        continue;
      const std::uint32_t stride = m.rows * m.columns * 4u;
      if (stride == 0)
        continue;
      const std::uint32_t end = std::min(sizeBytes, m.offset + m.sizeBytes);
      for (std::uint32_t off = m.offset; off + stride <= end; off += stride)
        writeIdentityMatrix(ptr, off, m.rows, m.columns);
    }
    // Give the first matrix-free 16-byte slot a plausible eye position. Several corpus
    // shaders divide by viewPos, and an all-zero vector sends them to NaN, which lands
    // as black pixels rather than an error. Skipped entirely when every slot is covered
    // by a matrix, since there is then nothing else in the block.
    std::vector<bool> taken(sizeBytes, false);
    for (const SpirvBlockMember &m : *matrices) {
      const std::uint32_t end = std::min(sizeBytes, m.offset + m.sizeBytes);
      for (std::uint32_t o = m.offset; o < end; ++o)
        taken[o] = true;
    }
    for (std::uint32_t o = 0; o + 16 <= sizeBytes; o += 16) {
      if (taken[o] || taken[o + 15])
        continue;
      auto *v = reinterpret_cast<float *>(static_cast<std::uint8_t *>(ptr) + o);
      v[0] = 0.0f;
      v[1] = 0.0f;
      v[2] = 2.0f;
      v[3] = 1.0f;
      break;
    }
    return;
  }
  auto *f = static_cast<float *>(ptr);
  // Identity for every full mat4 that fits.
  const std::uint32_t mats = sizeBytes / 64;
  for (std::uint32_t m = 0; m < mats; ++m) {
    float *mat = f + m * 16;
    mat[0] = mat[5] = mat[10] = mat[15] = 1.0f;
  }
  // Trailing vec4 (viewPos / lightPos) after the last mat4, if present.
  const std::uint32_t vecOff = mats * 64;
  if (vecOff + 16 <= sizeBytes) {
    float *v = reinterpret_cast<float *>(static_cast<std::uint8_t *>(ptr) + vecOff);
    v[0] = 0.0f;
    v[1] = 0.0f;
    v[2] = 2.0f;
    v[3] = 1.0f;
  }
}

/// Smoke defaults for push-constant blocks:
/// - ≥64 bytes: identity mat4s (MVP-style)
/// - smaller: first vec4 = white (common color tint); rest zero (offsets)
void fillSmokePushConstants(std::vector<std::uint8_t> &bytes) {
  if (bytes.empty())
    return;
  std::memset(bytes.data(), 0, bytes.size());
  if (bytes.size() >= 64) {
    fillIdentityUbo(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    return;
  }
  if (bytes.size() >= 16) {
    float *f = reinterpret_cast<float *>(bytes.data());
    f[0] = f[1] = f[2] = f[3] = 1.0f;
  }
}

VkFormat vertexFormat(const SpirvVertexAttribute &a) {
  if (a.type == SpirvVertexCompType::Int) {
    switch (a.components) {
    case 1:
      return VK_FORMAT_R32_SINT;
    case 2:
      return VK_FORMAT_R32G32_SINT;
    case 3:
      return VK_FORMAT_R32G32B32_SINT;
    default:
      return VK_FORMAT_R32G32B32A32_SINT;
    }
  }
  if (a.type == SpirvVertexCompType::Uint) {
    switch (a.components) {
    case 1:
      return VK_FORMAT_R32_UINT;
    case 2:
      return VK_FORMAT_R32G32_UINT;
    case 3:
      return VK_FORMAT_R32G32B32_UINT;
    default:
      return VK_FORMAT_R32G32B32A32_UINT;
    }
  }
  switch (a.components) {
  case 1:
    return VK_FORMAT_R32_SFLOAT;
  case 2:
    return VK_FORMAT_R32G32_SFLOAT;
  case 3:
    return VK_FORMAT_R32G32B32_SFLOAT;
  default:
    return VK_FORMAT_R32G32B32A32_SFLOAT;
  }
}

VkDescriptorType toVk(SpirvDescriptorKind k) {
  switch (k) {
  case SpirvDescriptorKind::UniformBuffer:
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  case SpirvDescriptorKind::StorageBuffer:
    return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  case SpirvDescriptorKind::CombinedImageSampler:
    return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  case SpirvDescriptorKind::SampledImage:
    return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  case SpirvDescriptorKind::Sampler:
    return VK_DESCRIPTOR_TYPE_SAMPLER;
  case SpirvDescriptorKind::StorageImage:
    return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  case SpirvDescriptorKind::InputAttachment:
    return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
  default:
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  }
}

VkShaderStageFlags stageFlags(const SpirvDescriptorBinding &b) {
  VkShaderStageFlags f = 0;
  if (b.fromVertex)
    f |= VK_SHADER_STAGE_VERTEX_BIT;
  if (b.fromFragment)
    f |= VK_SHADER_STAGE_FRAGMENT_BIT;
  if (b.fromGeometry)
    f |= VK_SHADER_STAGE_GEOMETRY_BIT;
  if (b.fromTessControl)
    f |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
  if (b.fromTessEvaluation)
    f |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  if (b.fromMesh)
    f |= VK_SHADER_STAGE_MESH_BIT_EXT;
  if (b.fromTask)
    f |= VK_SHADER_STAGE_TASK_BIT_EXT;
  if (f == 0)
    f = VK_SHADER_STAGE_ALL_GRAPHICS;
  return f;
}

} // namespace

Expected<VulkanGraphicsResources>
vulkanCreateGraphicsResources(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                              uint32_t queueFamily, const SpirvModuleReflection &refl,
                              std::vector<std::string> &steps) {
  VulkanGraphicsResources res;
  res.reflection = refl;

  // --- Vertex input layout + dummy triangle VBO ---
  if (!refl.vertexInputs.empty()) {
    res.hasVertexAttrs = true;
    std::uint32_t offset = 0;
    for (const auto &a : refl.vertexInputs) {
      VkVertexInputAttributeDescription ad{};
      ad.location = a.location;
      ad.binding = 0;
      ad.format = vertexFormat(a);
      ad.offset = offset;
      res.vertexAttrs.push_back(ad);
      offset += a.components * 4;
    }
    res.vertexStride = offset;
    VkVertexInputBindingDescription bd{};
    bd.binding = 0;
    bd.stride = res.vertexStride;
    bd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    res.vertexBindings.push_back(bd);

    // Three vertices: large triangle covering the viewport center in NDC.
    std::vector<float> data(res.vertexCount * (res.vertexStride / 4), 0.0f);
    const float positions[3][3] = {
        {-0.8f, -0.8f, 0.0f},
        {0.8f, -0.8f, 0.0f},
        {0.0f, 0.8f, 0.0f},
    };
    const float uvs[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    const float color[3] = {1.0f, 1.0f, 1.0f};

    for (std::uint32_t vi = 0; vi < res.vertexCount; ++vi) {
      float *v = data.data() + vi * (res.vertexStride / 4);
      std::uint32_t o = 0;
      for (const auto &a : refl.vertexInputs) {
        if (a.type != SpirvVertexCompType::Float) {
          o += a.components;
          continue;
        }
        if (a.components >= 3 && a.location == 0) {
          v[o + 0] = positions[vi][0];
          v[o + 1] = positions[vi][1];
          v[o + 2] = positions[vi][2];
          if (a.components == 4)
            v[o + 3] = 1.0f;
        } else if (a.components == 2) {
          v[o + 0] = uvs[vi][0];
          v[o + 1] = uvs[vi][1];
        } else if (a.components >= 3) {
          if (a.location == 1) {
            // Often color
            v[o + 0] = color[0];
            v[o + 1] = color[1];
            v[o + 2] = color[2];
          } else {
            // Normal facing the origin (many samples use lightPos=vec3(0)).
            const float nx = -positions[vi][0];
            const float ny = -positions[vi][1];
            const float nz = -positions[vi][2] + 1.0f;
            const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            v[o + 0] = nx * inv;
            v[o + 1] = ny * inv;
            v[o + 2] = nz * inv;
          }
          if (a.components == 4)
            v[o + 3] = 1.0f;
        } else if (a.components == 1) {
          v[o] = 0.0f;
        }
        o += a.components;
      }
    }

    void *mapped = nullptr;
    auto created =
        createHostBuffer(physical, device, data.size() * sizeof(float),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &res.vertexBuffer, &res.vertexMemory,
                         &mapped);
    if (!created) {
      vulkanDestroyGraphicsResources(device, res);
      return created.error();
    }
    std::memcpy(mapped, data.data(), data.size() * sizeof(float));
    vkUnmapMemory(device, res.vertexMemory);
    steps.push_back("auto vertex attrs: " + std::to_string(refl.vertexInputs.size()) +
                    " locations, stride=" + std::to_string(res.vertexStride));
  }

  // --- Push constants ---
  if (!refl.pushConstants.empty()) {
    std::uint32_t maxSize = 0;
    for (const auto &pc : refl.pushConstants) {
      maxSize = std::max(maxSize, pc.offset + pc.size);
      if (pc.fromVertex)
        res.pushStages |= VK_SHADER_STAGE_VERTEX_BIT;
      if (pc.fromFragment)
        res.pushStages |= VK_SHADER_STAGE_FRAGMENT_BIT;
      if (pc.fromGeometry)
        res.pushStages |= VK_SHADER_STAGE_GEOMETRY_BIT;
      if (pc.fromTessControl)
        res.pushStages |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
      if (pc.fromTessEvaluation)
        res.pushStages |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
      if (pc.fromMesh)
        res.pushStages |= VK_SHADER_STAGE_MESH_BIT_EXT;
      if (pc.fromTask)
        res.pushStages |= VK_SHADER_STAGE_TASK_BIT_EXT;
    }
    if (res.pushStages == 0)
      res.pushStages = VK_SHADER_STAGE_ALL_GRAPHICS;
    res.pushBytes.assign(maxSize, 0);
    bool hasBDA = false;
    for (const auto &pc : refl.pushConstants)
      if (pc.hasBufferReference)
        hasBDA = true;
    if (hasBDA) {
      VkBufferCreateInfo bci{};
      bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bci.size = 64;
      bci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      VkBuffer bdaBuf = VK_NULL_HANDLE;
      VkDeviceMemory bdaMem = VK_NULL_HANDLE;
      void *bdaMapped = nullptr;
      Expected<void> ebda = createHostBuffer(physical, device, 64,
                                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                            &bdaBuf, &bdaMem, &bdaMapped);
      if (ebda.hasValue()) {
        fillIdentityUbo(bdaMapped, 64);
        VkBufferDeviceAddressInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        dai.buffer = bdaBuf;
        VkDeviceAddress addr = vkGetBufferDeviceAddress(device, &dai);
        if (addr != 0) {
          std::memcpy(res.pushBytes.data(), &addr, sizeof(addr));
          if (res.pushBytes.size() >= 16)
            std::memcpy(res.pushBytes.data() + 8, &addr, sizeof(addr));
          res.bdaBuffers.push_back(bdaBuf);
          res.bdaMemories.push_back(bdaMem);
          steps.push_back("auto push constants: " + std::to_string(maxSize) +
                          " bytes (BDA device addresses)");
        } else {
          vkFreeMemory(device, bdaMem, nullptr);
          vkDestroyBuffer(device, bdaBuf, nullptr);
          fillSmokePushConstants(res.pushBytes);
          steps.push_back("auto push constants: " + std::to_string(maxSize) +
                          " bytes (smoke defaults)");
        }
      } else {
        fillSmokePushConstants(res.pushBytes);
        steps.push_back("auto push constants: " + std::to_string(maxSize) +
                        " bytes (smoke defaults)");
      }
    } else {
      fillSmokePushConstants(res.pushBytes);
      steps.push_back("auto push constants: " + std::to_string(maxSize) + " bytes (smoke defaults)");
    }
  }

  // --- subpass inputs ---
  //
  // The views these descriptors point at are render pass attachments, which the caller
  // creates after layout/pool/set allocation, so only the bindings are recorded here and
  // the writes happen in vulkanWriteInputAttachmentDescriptors().
  for (const auto &b : refl.descriptors) {
    if (b.kind == SpirvDescriptorKind::InputAttachment)
      res.inputAttachments.push_back(b);
  }

  // --- Descriptor set layouts (sets 0..maxSet, empty layouts fill gaps) ---
  constexpr std::uint32_t kMaxDescriptorSets = 8;
  std::uint32_t maxSet = 0;
  bool anyDesc = false;
  for (const auto &b : refl.descriptors) {
    anyDesc = true;
    maxSet = std::max(maxSet, b.set);
  }
  if (anyDesc && maxSet >= kMaxDescriptorSets) {
    vulkanDestroyGraphicsResources(device, res);
    return Error::make(ErrorCode::UnsupportedOperation,
                       "graphics smoke supports at most " + std::to_string(kMaxDescriptorSets) +
                           " descriptor sets (shader uses set " + std::to_string(maxSet) + ")");
  }

  const std::uint32_t setCount = anyDesc ? (maxSet + 1) : 0;
  std::vector<std::vector<VkDescriptorSetLayoutBinding>> bindingsPerSet(setCount);
  for (const auto &b : refl.descriptors) {
    if (b.kind == SpirvDescriptorKind::Unknown) {
      vulkanDestroyGraphicsResources(device, res);
      return Error::make(ErrorCode::UnsupportedOperation,
                         "unsupported descriptor kind at set=" + std::to_string(b.set) +
                             " binding=" + std::to_string(b.binding));
    }
    if (b.kind == SpirvDescriptorKind::StorageImage) {
      vulkanDestroyGraphicsResources(device, res);
      return Error::make(ErrorCode::UnsupportedOperation,
                         "graphics smoke does not bind storage images yet (set=" +
                             std::to_string(b.set) + " binding=" + std::to_string(b.binding) + ")");
    }
    VkDescriptorSetLayoutBinding lb{};
    lb.binding = b.binding;
    lb.descriptorType = toVk(b.kind);
    lb.descriptorCount = b.count;
    lb.stageFlags = stageFlags(b);
    bindingsPerSet[b.set].push_back(lb);
  }

  res.setLayouts.assign(setCount, VK_NULL_HANDLE);
  for (std::uint32_t s = 0; s < setCount; ++s) {
    res.hasDescriptors = true;
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = static_cast<uint32_t>(bindingsPerSet[s].size());
    dslci.pBindings = bindingsPerSet[s].empty() ? nullptr : bindingsPerSet[s].data();
    VkResult r = vkCreateDescriptorSetLayout(device, &dslci, nullptr, &res.setLayouts[s]);
    if (r != VK_SUCCESS) {
      vulkanDestroyGraphicsResources(device, res);
      return vkErr("vkCreateDescriptorSetLayout failed", r);
    }
  }

  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  if (!res.setLayouts.empty()) {
    plci.setLayoutCount = static_cast<uint32_t>(res.setLayouts.size());
    plci.pSetLayouts = res.setLayouts.data();
  }
  VkPushConstantRange pcr{};
  if (!res.pushBytes.empty()) {
    pcr.stageFlags = res.pushStages;
    pcr.offset = 0;
    pcr.size = static_cast<uint32_t>(res.pushBytes.size());
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
  }
  VkResult r = vkCreatePipelineLayout(device, &plci, nullptr, &res.pipelineLayout);
  if (r != VK_SUCCESS) {
    vulkanDestroyGraphicsResources(device, res);
    return vkErr("vkCreatePipelineLayout failed", r);
  }

  if (!anyDesc) {
    steps.push_back("auto graphics layout: empty descriptors (gl_VertexIndex style OK)");
    return res;
  }

  // Pool sizes across all sets
  std::unordered_map<VkDescriptorType, uint32_t> poolCounts;
  for (const auto &setBindings : bindingsPerSet)
    for (const auto &lb : setBindings)
      poolCounts[lb.descriptorType] += lb.descriptorCount;
  // Empty sets still need a pool entry for allocate; Vulkan allows maxSets with empty layouts.
  if (poolCounts.empty())
    poolCounts[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER] = 1;
  std::vector<VkDescriptorPoolSize> poolSizes;
  for (const auto &kv : poolCounts)
    poolSizes.push_back(VkDescriptorPoolSize{kv.first, std::max(kv.second, 1u)});

  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = setCount;
  dpci.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  dpci.pPoolSizes = poolSizes.data();
  r = vkCreateDescriptorPool(device, &dpci, nullptr, &res.descPool);
  if (r != VK_SUCCESS) {
    vulkanDestroyGraphicsResources(device, res);
    return vkErr("vkCreateDescriptorPool failed", r);
  }

  res.descSets.assign(setCount, VK_NULL_HANDLE);
  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = res.descPool;
  dsai.descriptorSetCount = setCount;
  dsai.pSetLayouts = res.setLayouts.data();
  r = vkAllocateDescriptorSets(device, &dsai, res.descSets.data());
  if (r != VK_SUCCESS) {
    vulkanDestroyGraphicsResources(device, res);
    return vkErr("vkAllocateDescriptorSets failed", r);
  }

  // Need texture?
  bool needTex = false;
  for (const auto &b : refl.descriptors) {
    if (b.kind == SpirvDescriptorKind::CombinedImageSampler ||
        b.kind == SpirvDescriptorKind::SampledImage || b.kind == SpirvDescriptorKind::Sampler)
      needTex = true;
  }

  if (needTex) {
    // 4x4 white RGBA8 — optimal tiling + staging copy + layout transition.
    constexpr uint32_t tw = 4, th = 4;
    std::vector<std::uint8_t> pixels(tw * th * 4, 255);

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void *stagingMap = nullptr;
    auto st = createHostBuffer(physical, device, pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               &staging, &stagingMem, &stagingMap);
    if (!st) {
      vulkanDestroyGraphicsResources(device, res);
      return st.error();
    }
    std::memcpy(stagingMap, pixels.data(), pixels.size());
    vkUnmapMemory(device, stagingMem);

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {tw, th, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = vkCreateImage(device, &ici, nullptr, &res.texImage);
    if (r != VK_SUCCESS) {
      vkFreeMemory(device, stagingMem, nullptr);
      vkDestroyBuffer(device, staging, nullptr);
      vulkanDestroyGraphicsResources(device, res);
      return vkErr("vkCreateImage (tex) failed", r);
    }
    VkMemoryRequirements ireq{};
    vkGetImageMemoryRequirements(device, res.texImage, &ireq);
    const uint32_t mt =
        findMem(physical, ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == std::numeric_limits<uint32_t>::max()) {
      vkFreeMemory(device, stagingMem, nullptr);
      vkDestroyBuffer(device, staging, nullptr);
      vulkanDestroyGraphicsResources(device, res);
      return Error::make(ErrorCode::ResourceAllocationFailed, "no device-local mem for texture");
    }
    VkMemoryAllocateInfo iai{};
    iai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    iai.allocationSize = ireq.size;
    iai.memoryTypeIndex = mt;
    r = vkAllocateMemory(device, &iai, nullptr, &res.texMemory);
    if (r != VK_SUCCESS) {
      vkFreeMemory(device, stagingMem, nullptr);
      vkDestroyBuffer(device, staging, nullptr);
      vulkanDestroyGraphicsResources(device, res);
      return vkErr("vkAllocateMemory (tex) failed", r);
    }
    vkBindImageMemory(device, res.texImage, res.texMemory, 0);

    VkCommandPoolCreateInfo poolCi{};
    poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCi.queueFamilyIndex = queueFamily;
    poolCi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool uploadPool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &poolCi, nullptr, &uploadPool);
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = uploadPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer upCmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cbai, &upCmd);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(upCmd, &begin);
    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = res.texImage;
    toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDst.subresourceRange.levelCount = 1;
    toDst.subresourceRange.layerCount = 1;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(upCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toDst);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {tw, th, 1};
    vkCmdCopyBufferToImage(upCmd, staging, res.texImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);
    VkImageMemoryBarrier toSample{};
    toSample.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSample.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSample.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSample.image = res.texImage;
    toSample.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toSample.subresourceRange.levelCount = 1;
    toSample.subresourceRange.layerCount = 1;
    toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(upCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toSample);
    vkEndCommandBuffer(upCmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &upCmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkDestroyCommandPool(device, uploadPool, nullptr);
    vkFreeMemory(device, stagingMem, nullptr);
    vkDestroyBuffer(device, staging, nullptr);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = res.texImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    r = vkCreateImageView(device, &vci, nullptr, &res.texView);
    if (r != VK_SUCCESS) {
      vulkanDestroyGraphicsResources(device, res);
      return vkErr("vkCreateImageView (tex) failed", r);
    }
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 1.0f;
    r = vkCreateSampler(device, &sci, nullptr, &res.texSampler);
    if (r != VK_SUCCESS) {
      vulkanDestroyGraphicsResources(device, res);
      return vkErr("vkCreateSampler failed", r);
    }
    steps.push_back("auto white texture 4x4 for samplers");
  }

  // Create UBO/SSBO buffers + writes
  std::vector<VkWriteDescriptorSet> writes;
  std::vector<VkDescriptorBufferInfo> bufInfos;
  std::vector<VkDescriptorImageInfo> imgInfos;
  std::size_t imgSlots = 0;
  for (const auto &b : refl.descriptors) {
    if (b.kind == SpirvDescriptorKind::CombinedImageSampler ||
        b.kind == SpirvDescriptorKind::SampledImage || b.kind == SpirvDescriptorKind::Sampler)
      imgSlots += std::max(b.count, 1u);
  }
  bufInfos.reserve(refl.descriptors.size());
  imgInfos.reserve(imgSlots);
  writes.reserve(refl.descriptors.size());

  for (const auto &b : refl.descriptors) {
    if (b.kind == SpirvDescriptorKind::UniformBuffer ||
        b.kind == SpirvDescriptorKind::StorageBuffer) {
      const VkDeviceSize sz = std::max<std::uint32_t>(b.sizeBytes, 64);
      VkBuffer buf = VK_NULL_HANDLE;
      VkDeviceMemory mem = VK_NULL_HANDLE;
      void *mapped = nullptr;
      const VkBufferUsageFlags usage = (b.kind == SpirvDescriptorKind::UniformBuffer)
                                           ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                                           : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      auto created = createHostBuffer(physical, device, sz, usage, &buf, &mem, &mapped);
      if (!created) {
        vulkanDestroyGraphicsResources(device, res);
        return created.error();
      }
      fillIdentityUbo(mapped, static_cast<std::uint32_t>(sz), &b.members);
      vkUnmapMemory(device, mem);
      if (b.kind == SpirvDescriptorKind::UniformBuffer) {
        res.uboBuffers.push_back(buf);
        res.uboMemories.push_back(mem);
      } else {
        res.ssboBuffers.push_back(buf);
        res.ssboMemories.push_back(mem);
      }
      VkDescriptorBufferInfo bi{};
      bi.buffer = buf;
      bi.offset = 0;
      bi.range = sz;
      bufInfos.push_back(bi);
      VkWriteDescriptorSet w{};
      w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w.dstSet = res.descSets[b.set];
      w.dstBinding = b.binding;
      w.descriptorCount = 1;
      w.descriptorType = toVk(b.kind);
      w.pBufferInfo = &bufInfos.back();
      writes.push_back(w);
    } else if (b.kind == SpirvDescriptorKind::CombinedImageSampler ||
               b.kind == SpirvDescriptorKind::SampledImage ||
               b.kind == SpirvDescriptorKind::Sampler) {
      const uint32_t n = std::max(b.count, 1u);
      const size_t base = imgInfos.size();
      for (uint32_t i = 0; i < n; ++i) {
        VkDescriptorImageInfo ii{};
        ii.sampler = res.texSampler;
        ii.imageView = res.texView;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfos.push_back(ii);
      }
      VkWriteDescriptorSet w{};
      w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w.dstSet = res.descSets[b.set];
      w.dstBinding = b.binding;
      w.descriptorCount = n;
      w.descriptorType = toVk(b.kind);
      w.pImageInfo = &imgInfos[base];
      writes.push_back(w);
    }
  }

  if (!writes.empty())
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

  steps.push_back("auto descriptors: " + std::to_string(refl.descriptors.size()) +
                  " bindings across " + std::to_string(setCount) + " set(s)");
  return res;
}

void vulkanDestroyGraphicsResources(VkDevice device, VulkanGraphicsResources &res) {
  if (res.texSampler)
    vkDestroySampler(device, res.texSampler, nullptr);
  if (res.texView)
    vkDestroyImageView(device, res.texView, nullptr);
  if (res.texMemory)
    vkFreeMemory(device, res.texMemory, nullptr);
  if (res.texImage)
    vkDestroyImage(device, res.texImage, nullptr);
  for (auto m : res.uboMemories)
    vkFreeMemory(device, m, nullptr);
  for (auto b : res.uboBuffers)
    vkDestroyBuffer(device, b, nullptr);
  for (auto m : res.ssboMemories)
    vkFreeMemory(device, m, nullptr);
  for (auto b : res.ssboBuffers)
    vkDestroyBuffer(device, b, nullptr);
  for (auto m : res.bdaMemories)
    vkFreeMemory(device, m, nullptr);
  for (auto b : res.bdaBuffers)
    vkDestroyBuffer(device, b, nullptr);
  if (res.vertexMemory)
    vkFreeMemory(device, res.vertexMemory, nullptr);
  if (res.vertexBuffer)
    vkDestroyBuffer(device, res.vertexBuffer, nullptr);
  if (res.descPool)
    vkDestroyDescriptorPool(device, res.descPool, nullptr);
  if (res.pipelineLayout)
    vkDestroyPipelineLayout(device, res.pipelineLayout, nullptr);
  for (VkDescriptorSetLayout layout : res.setLayouts) {
    if (layout)
      vkDestroyDescriptorSetLayout(device, layout, nullptr);
  }
  res = VulkanGraphicsResources{};
}

Expected<void> vulkanWriteInputAttachmentDescriptors(VkDevice device, VulkanGraphicsResources &res,
                                                     const std::vector<VkImageView> &views) {
  if (res.inputAttachments.empty())
    return {};
  if (views.empty()) {
    return Error::make(ErrorCode::UnsupportedOperation,
                       "shader declares subpass inputs but the render pass created no input "
                       "attachment views");
  }

  std::vector<VkDescriptorImageInfo> imgInfos;
  std::vector<VkWriteDescriptorSet> writes;
  // Reserve up front: VkWriteDescriptorSet::pImageInfo points into imgInfos, so a
  // reallocation while the writes are being built leaves those pointers dangling.
  size_t totalImages = 0;
  for (const SpirvDescriptorBinding &b : res.inputAttachments)
    totalImages += std::max(b.count, 1u);
  imgInfos.reserve(totalImages);
  for (const SpirvDescriptorBinding &b : res.inputAttachments) {
    if (b.set >= res.descSets.size() || !res.descSets[b.set]) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "subpass input references set=" + std::to_string(b.set) +
                             " binding=" + std::to_string(b.binding) + " with no descriptor set");
    }
    // A `subpassInput` array carries one InputAttachmentIndex for the whole array, so every
    // element reads the same attachment. An index past the last view falls back to 0 rather
    // than reading off the end.
    const size_t slot = b.inputAttachmentIndex < views.size() ? b.inputAttachmentIndex : 0u;
    const uint32_t n = std::max(b.count, 1u);
    const size_t base = imgInfos.size();
    for (uint32_t i = 0; i < n; ++i) {
      VkDescriptorImageInfo ii{};
      ii.sampler = VK_NULL_HANDLE;
      ii.imageView = views[slot];
      // Must match the VkAttachmentReference layout of the consuming subpass.
      ii.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      imgInfos.push_back(ii);
    }
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = res.descSets[b.set];
    w.dstBinding = b.binding;
    w.descriptorCount = n;
    w.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    w.pImageInfo = &imgInfos[base];
    writes.push_back(w);
  }

  vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  return {};
}

void vulkanCmdBindGraphicsResources(VkCommandBuffer cmd, const VulkanGraphicsResources &res) {
  if (res.hasDescriptors && !res.descSets.empty() && res.pipelineLayout) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res.pipelineLayout, 0,
                            static_cast<uint32_t>(res.descSets.size()), res.descSets.data(), 0,
                            nullptr);
  }
  if (!res.pushBytes.empty() && res.pipelineLayout) {
    vkCmdPushConstants(cmd, res.pipelineLayout, res.pushStages, 0,
                       static_cast<uint32_t>(res.pushBytes.size()), res.pushBytes.data());
  }
  if (res.hasVertexAttrs && res.vertexBuffer) {
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &res.vertexBuffer, &off);
  }
}

} // namespace csrun
