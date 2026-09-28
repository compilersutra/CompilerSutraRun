//===----------------------------------------------------------------------===//
//
// VulkanComputeResources.cpp
//
//===----------------------------------------------------------------------===//

#include "csrun/backends/vulkan/VulkanComputeResources.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace csrun {
namespace {

Error vkErr(const std::string &msg, VkResult r) {
  Error e = Error::make(ErrorCode::DriverError, msg + " (VkResult=" + std::to_string(int(r)) + ")");
  e.backend = "vulkan";
  e.operation = "execute";
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

VkDescriptorType toVk(SpirvDescriptorKind k) {
  switch (k) {
  case SpirvDescriptorKind::UniformBuffer:
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  case SpirvDescriptorKind::StorageBuffer:
    return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  case SpirvDescriptorKind::StorageImage:
    return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  case SpirvDescriptorKind::CombinedImageSampler:
    return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  case SpirvDescriptorKind::SampledImage:
    return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  case SpirvDescriptorKind::Sampler:
    return VK_DESCRIPTOR_TYPE_SAMPLER;
  default:
    return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  }
}

void fillIdentity(void *ptr, std::uint32_t sizeBytes) {
  std::memset(ptr, 0, sizeBytes);
  auto *f = static_cast<float *>(ptr);
  const std::uint32_t mats = sizeBytes / 64;
  for (std::uint32_t m = 0; m < mats; ++m) {
    float *mat = f + m * 16;
    mat[0] = mat[5] = mat[10] = mat[15] = 1.0f;
  }
}

Expected<void> createHostBuf(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                             VkBufferUsageFlags usage, VkBuffer *outBuf, VkDeviceMemory *outMem,
                             void **mapped) {
  VkBufferCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  ci.size = size;
  ci.usage = usage;
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

} // namespace

bool vulkanComputeNeedsGenericResources(const SpirvModuleReflection &refl) {
  if (refl.descriptors.empty())
    return false;
  for (const auto &b : refl.descriptors) {
    if (b.set > 0)
      return true;
    if (b.kind != SpirvDescriptorKind::StorageBuffer)
      return true;
  }
  return false;
}

Expected<void> vulkanRunComputeGeneric(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                                       uint32_t queueFamily, const std::vector<uint32_t> &words,
                                       const VulkanSmokeRequest &req, VulkanSmokeResult &out) {
  auto &steps = out.steps;
  const uint32_t extentW = req.width > 0 ? req.width : 64;
  const uint32_t extentH = req.height > 0 ? req.height : 64;

  auto reflOr = reflectSpirvModule(words, SpirvReflectStage::Fragment);
  if (!reflOr)
    return reflOr.error();
  const SpirvModuleReflection &refl = reflOr.value();

  for (const auto &b : refl.descriptors) {
    if (b.kind == SpirvDescriptorKind::Unknown) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "unsupported descriptor kind at set=" + std::to_string(b.set) +
                             " binding=" + std::to_string(b.binding));
    }
    if (b.kind == SpirvDescriptorKind::CombinedImageSampler ||
        b.kind == SpirvDescriptorKind::SampledImage || b.kind == SpirvDescriptorKind::Sampler) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "compute generic smoke does not bind sampled images yet (set=" +
                             std::to_string(b.set) + " binding=" + std::to_string(b.binding) + ")");
    }
    if (b.kind == SpirvDescriptorKind::InputAttachment) {
      // A compute module cannot read a render pass attachment. Reject rather than let the
      // generic path fall through to its default descriptor type.
      return Error::make(ErrorCode::UnsupportedOperation,
                         "input attachments are a graphics-only feature (set=" +
                             std::to_string(b.set) + " binding=" + std::to_string(b.binding) + ")");
    }
  }

  constexpr uint32_t kMaxSets = 8;
  uint32_t maxSet = 0;
  for (const auto &b : refl.descriptors)
    maxSet = std::max(maxSet, b.set);
  if (maxSet >= kMaxSets) {
    return Error::make(ErrorCode::UnsupportedOperation,
                       "compute smoke supports at most " + std::to_string(kMaxSets) +
                           " descriptor sets");
  }
  const uint32_t setCount = maxSet + 1;

  VkShaderModuleCreateInfo mci{};
  mci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  mci.codeSize = words.size() * sizeof(uint32_t);
  mci.pCode = words.data();
  VkShaderModule mod = VK_NULL_HANDLE;
  VkResult r = vkCreateShaderModule(device, &mci, nullptr, &mod);
  if (r != VK_SUCCESS)
    return vkErr("vkCreateShaderModule failed", r);
  steps.push_back("vkCreateShaderModule (generic compute)");

  std::vector<std::vector<VkDescriptorSetLayoutBinding>> bindingsPerSet(setCount);
  for (const auto &b : refl.descriptors) {
    VkDescriptorSetLayoutBinding lb{};
    lb.binding = b.binding;
    lb.descriptorType = toVk(b.kind);
    lb.descriptorCount = std::max(b.count, 1u);
    lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindingsPerSet[b.set].push_back(lb);
  }

  std::vector<VkDescriptorSetLayout> setLayouts(setCount, VK_NULL_HANDLE);
  for (uint32_t s = 0; s < setCount; ++s) {
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = static_cast<uint32_t>(bindingsPerSet[s].size());
    dslci.pBindings = bindingsPerSet[s].empty() ? nullptr : bindingsPerSet[s].data();
    r = vkCreateDescriptorSetLayout(device, &dslci, nullptr, &setLayouts[s]);
    if (r != VK_SUCCESS) {
      for (auto l : setLayouts)
        if (l)
          vkDestroyDescriptorSetLayout(device, l, nullptr);
      vkDestroyShaderModule(device, mod, nullptr);
      return vkErr("vkCreateDescriptorSetLayout failed", r);
    }
  }

  uint32_t pushSize = 0;
  for (const auto &pc : refl.pushConstants)
    pushSize = std::max(pushSize, pc.offset + pc.size);
  std::vector<uint8_t> pushBytes(pushSize, 0);
  if (pushSize >= 16) {
    float *f = reinterpret_cast<float *>(pushBytes.data());
    f[0] = f[1] = f[2] = f[3] = 1.0f;
  }

  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcr.offset = 0;
  pcr.size = pushSize;

  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = setCount;
  plci.pSetLayouts = setLayouts.data();
  if (pushSize > 0) {
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
  }
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  r = vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout);
  if (r != VK_SUCCESS) {
    for (auto l : setLayouts)
      vkDestroyDescriptorSetLayout(device, l, nullptr);
    vkDestroyShaderModule(device, mod, nullptr);
    return vkErr("vkCreatePipelineLayout failed", r);
  }

  VkComputePipelineCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = mod;
  cpci.stage.pName = "main";
  cpci.layout = pipelineLayout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
  if (r != VK_SUCCESS) {
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    for (auto l : setLayouts)
      vkDestroyDescriptorSetLayout(device, l, nullptr);
    vkDestroyShaderModule(device, mod, nullptr);
    return vkErr("vkCreateComputePipelines failed", r);
  }

  std::unordered_map<VkDescriptorType, uint32_t> poolCounts;
  for (const auto &b : refl.descriptors)
    poolCounts[toVk(b.kind)] += std::max(b.count, 1u);
  if (poolCounts.empty())
    poolCounts[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER] = 1;
  std::vector<VkDescriptorPoolSize> poolSizes;
  for (const auto &kv : poolCounts)
    poolSizes.push_back(VkDescriptorPoolSize{kv.first, std::max(kv.second, 1u)});

  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = setCount;
  dpci.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  dpci.pPoolSizes = poolSizes.data();
  VkDescriptorPool descPool = VK_NULL_HANDLE;
  r = vkCreateDescriptorPool(device, &dpci, nullptr, &descPool);
  if (r != VK_SUCCESS) {
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    for (auto l : setLayouts)
      vkDestroyDescriptorSetLayout(device, l, nullptr);
    vkDestroyShaderModule(device, mod, nullptr);
    return vkErr("vkCreateDescriptorPool failed", r);
  }

  std::vector<VkDescriptorSet> descSets(setCount, VK_NULL_HANDLE);
  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = descPool;
  dsai.descriptorSetCount = setCount;
  dsai.pSetLayouts = setLayouts.data();
  r = vkAllocateDescriptorSets(device, &dsai, descSets.data());
  if (r != VK_SUCCESS) {
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    for (auto l : setLayouts)
      vkDestroyDescriptorSetLayout(device, l, nullptr);
    vkDestroyShaderModule(device, mod, nullptr);
    return vkErr("vkAllocateDescriptorSets failed", r);
  }

  std::vector<VkBuffer> buffers;
  std::vector<VkDeviceMemory> memories;
  std::vector<VkImage> images;
  std::vector<VkDeviceMemory> imageMems;
  std::vector<VkImageView> imageViews;
  std::vector<VkDescriptorBufferInfo> bufInfos;
  std::vector<VkDescriptorImageInfo> imgInfos;
  std::vector<VkWriteDescriptorSet> writes;
  bufInfos.reserve(refl.descriptors.size());
  imgInfos.reserve(refl.descriptors.size());
  writes.reserve(refl.descriptors.size());

  auto cleanupAll = [&]() {
    for (auto v : imageViews)
      vkDestroyImageView(device, v, nullptr);
    for (auto m : imageMems)
      vkFreeMemory(device, m, nullptr);
    for (auto i : images)
      vkDestroyImage(device, i, nullptr);
    for (auto m : memories)
      vkFreeMemory(device, m, nullptr);
    for (auto b : buffers)
      vkDestroyBuffer(device, b, nullptr);
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    for (auto l : setLayouts)
      vkDestroyDescriptorSetLayout(device, l, nullptr);
    vkDestroyShaderModule(device, mod, nullptr);
  };

  for (const auto &b : refl.descriptors) {
    if (b.kind == SpirvDescriptorKind::UniformBuffer ||
        b.kind == SpirvDescriptorKind::StorageBuffer) {
      const VkDeviceSize sz =
          std::max<VkDeviceSize>(b.sizeBytes ? b.sizeBytes : 256, 256);
      VkBuffer buf = VK_NULL_HANDLE;
      VkDeviceMemory mem = VK_NULL_HANDLE;
      void *mapped = nullptr;
      const VkBufferUsageFlags usage = (b.kind == SpirvDescriptorKind::UniformBuffer)
                                           ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                                           : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      auto created = createHostBuf(physical, device, sz, usage, &buf, &mem, &mapped);
      if (!created) {
        cleanupAll();
        return created.error();
      }
      if (b.kind == SpirvDescriptorKind::UniformBuffer)
        fillIdentity(mapped, static_cast<uint32_t>(sz));
      else
        std::memset(mapped, 0, static_cast<size_t>(sz));
      vkUnmapMemory(device, mem);
      buffers.push_back(buf);
      memories.push_back(mem);
      VkDescriptorBufferInfo bi{};
      bi.buffer = buf;
      bi.range = sz;
      bufInfos.push_back(bi);
      VkWriteDescriptorSet w{};
      w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w.dstSet = descSets[b.set];
      w.dstBinding = b.binding;
      w.descriptorCount = 1;
      w.descriptorType = toVk(b.kind);
      w.pBufferInfo = &bufInfos.back();
      writes.push_back(w);
    } else if (b.kind == SpirvDescriptorKind::StorageImage) {
      VkImageCreateInfo ici{};
      ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      ici.imageType = VK_IMAGE_TYPE_2D;
      ici.format = VK_FORMAT_R8G8B8A8_UNORM;
      ici.extent = {extentW, extentH, 1};
      ici.mipLevels = 1;
      ici.arrayLayers = 1;
      ici.samples = VK_SAMPLE_COUNT_1_BIT;
      ici.tiling = VK_IMAGE_TILING_OPTIMAL;
      ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VkImage img = VK_NULL_HANDLE;
      r = vkCreateImage(device, &ici, nullptr, &img);
      if (r != VK_SUCCESS) {
        cleanupAll();
        return vkErr("vkCreateImage (storage) failed", r);
      }
      VkMemoryRequirements ireq{};
      vkGetImageMemoryRequirements(device, img, &ireq);
      const uint32_t mt = findMem(physical, ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (mt == std::numeric_limits<uint32_t>::max()) {
        vkDestroyImage(device, img, nullptr);
        cleanupAll();
        return Error::make(ErrorCode::ResourceAllocationFailed, "no device-local mem for storage image");
      }
      VkMemoryAllocateInfo iai{};
      iai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      iai.allocationSize = ireq.size;
      iai.memoryTypeIndex = mt;
      VkDeviceMemory imem = VK_NULL_HANDLE;
      r = vkAllocateMemory(device, &iai, nullptr, &imem);
      if (r != VK_SUCCESS) {
        vkDestroyImage(device, img, nullptr);
        cleanupAll();
        return vkErr("vkAllocateMemory (storage image) failed", r);
      }
      vkBindImageMemory(device, img, imem, 0);

      // Transition UNDEFINED → GENERAL for storage.
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
      VkImageMemoryBarrier bar{};
      bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      bar.image = img;
      bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      bar.subresourceRange.levelCount = 1;
      bar.subresourceRange.layerCount = 1;
      bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      vkCmdPipelineBarrier(upCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
      vkEndCommandBuffer(upCmd);
      VkSubmitInfo submit{};
      submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &upCmd;
      vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
      vkQueueWaitIdle(queue);
      vkDestroyCommandPool(device, uploadPool, nullptr);

      VkImageViewCreateInfo vci{};
      vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      vci.image = img;
      vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vci.format = VK_FORMAT_R8G8B8A8_UNORM;
      vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      vci.subresourceRange.levelCount = 1;
      vci.subresourceRange.layerCount = 1;
      VkImageView view = VK_NULL_HANDLE;
      r = vkCreateImageView(device, &vci, nullptr, &view);
      if (r != VK_SUCCESS) {
        vkFreeMemory(device, imem, nullptr);
        vkDestroyImage(device, img, nullptr);
        cleanupAll();
        return vkErr("vkCreateImageView (storage) failed", r);
      }
      images.push_back(img);
      imageMems.push_back(imem);
      imageViews.push_back(view);

      VkDescriptorImageInfo ii{};
      ii.imageView = view;
      ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      imgInfos.push_back(ii);
      VkWriteDescriptorSet w{};
      w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w.dstSet = descSets[b.set];
      w.dstBinding = b.binding;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      w.pImageInfo = &imgInfos.back();
      writes.push_back(w);
    }
  }

  if (!writes.empty())
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
  steps.push_back("generic compute descriptors: " + std::to_string(refl.descriptors.size()) +
                  " bindings across " + std::to_string(setCount) + " set(s)");

  VkCommandPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolCi.queueFamilyIndex = queueFamily;
  poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool cmdPool = VK_NULL_HANDLE;
  r = vkCreateCommandPool(device, &poolCi, nullptr, &cmdPool);
  if (r != VK_SUCCESS) {
    cleanupAll();
    return vkErr("vkCreateCommandPool failed", r);
  }
  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = cmdPool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(device, &cbai, &cmd);

  const int timedIters = std::max(1, req.iterations);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < timedIters; ++i) {
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, setCount,
                            descSets.data(), 0, nullptr);
    if (pushSize > 0)
      vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize,
                         pushBytes.data());
    // 16x16 workgroups cover the smoke extent (matches edgedetect local_size).
    const uint32_t gx = std::max(1u, (extentW + 15u) / 16u);
    const uint32_t gy = std::max(1u, (extentH + 15u) / 16u);
    vkCmdDispatch(cmd, gx, gy, 1);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    r = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
      vkDestroyCommandPool(device, cmdPool, nullptr);
      cleanupAll();
      return vkErr("vkQueueSubmit failed", r);
    }
    vkQueueWaitIdle(queue);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double hostMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  out.hostMs = hostMs;
  out.samplesMs.push_back(hostMs / timedIters);
  out.timingSource = TimingSource::CpuWall;
  out.executed = true;
  out.ok = true;
  out.computeChecked = true;
  out.mode = "compute_dispatch";
  steps.push_back("generic compute dispatch " + std::to_string(extentW) + "x" +
                  std::to_string(extentH) + " groups=" + std::to_string((extentW + 15) / 16) + "x" +
                  std::to_string((extentH + 15) / 16));

  vkDestroyCommandPool(device, cmdPool, nullptr);
  cleanupAll();
  return {};
}

} // namespace csrun
