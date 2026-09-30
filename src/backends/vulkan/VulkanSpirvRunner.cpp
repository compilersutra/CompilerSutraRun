#include "csrun/backends/vulkan/VulkanSpirvRunner.h"

#include "csrun/backends/vulkan/VulkanDevicePick.h"
#include "csrun/backends/vulkan/VulkanComputeResources.h"
#include "csrun/backends/vulkan/VulkanGraphicsResources.h"
#include "csrun/backends/vulkan/VulkanPerfQuery.h"
#include "csrun/backends/vulkan/VulkanPipelineQueries.h"
#include "csrun/backends/vulkan/VulkanWsiPresenter.h"
#include "csrun/compiler/SpirvReflect.h"
#include "csrun/compiler/SpirvToolchain.h"
#include "csrun/core/Log.h"
#include "csrun/validation/FrameCheck.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace csrun {
namespace {

constexpr uint32_t kDefaultExtent = 64;

std::string vkResultString(VkResult r) {
  return "VkResult=" + std::to_string(static_cast<int>(r));
}

Error vkError(const std::string &msg, VkResult r) {
  Error e = Error::make(ErrorCode::DriverError, msg + " (" + vkResultString(r) + ")");
  e.backend = "vulkan";
  return e;
}

Expected<std::vector<uint32_t>> loadWords(const std::filesystem::path &path) {
  return SpirvToolchain::loadSpvFile(path);
}

Expected<VkShaderModule> createModule(VkDevice device, const std::vector<uint32_t> &words,
                                       std::vector<std::string> &steps) {
  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = words.size() * sizeof(uint32_t);
  ci.pCode = words.data();
  VkShaderModule mod = VK_NULL_HANDLE;
  VkResult r = vkCreateShaderModule(device, &ci, nullptr, &mod);
  if (r != VK_SUCCESS)
    return vkError("vkCreateShaderModule failed", r);
  steps.push_back("vkCreateShaderModule");
  return mod;
}

uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags props) {
  VkPhysicalDeviceMemoryProperties memProps{};
  vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) &&
        (memProps.memoryTypes[i].propertyFlags & props) == props)
      return i;
  }
  return std::numeric_limits<uint32_t>::max();
}

/// Upper bound on primed subpass-input attachments. `MAX_COLOR_ATTACHMENTS` is 8 and a
/// `subpassInput` index has to be addressable, so anything beyond that is out of scope for
/// the smoke path rather than silently dropped.
constexpr uint32_t kMaxInputAttachments = 8;

/// Upper bound on colour attachments. Matches `MAX_COLOR_ATTACHMENTS` on every known
/// implementation, so a shader declaring more outputs is rejected loudly instead of being
/// run with the extra locations left undefined.
constexpr uint32_t kMaxColorOutputs = 8;

/// How many attachments the smoke render pass primes for `subpassInput` reads: one per
/// declared InputAttachmentIndex, so a shader reading index 2 gets a real attachment 2.
uint32_t inputAttachmentCount(const std::vector<SpirvDescriptorBinding> &binds) {
  uint32_t count = 0;
  for (const SpirvDescriptorBinding &b : binds)
    count = std::max(count, b.inputAttachmentIndex + 1);
  return std::min(count, kMaxInputAttachments);
}

/// Value the priming subpass clears an input attachment to: a mid-grey ramp, distinct per
/// index so a shader that reads only one of them still lands on a defined, non-zero value.
/// This is placeholder data, not a G-buffer — the point is to prove the subpass read works.
float inputAttachmentPrimeValue(uint32_t index, uint32_t count) {
  return 0.25f + 0.5f * static_cast<float>(index + 1) / static_cast<float>(count);
}

} // namespace

Expected<VulkanSmokeResult> vulkanSmokeSpirv(const VulkanSmokeRequest &req) {
  const bool wantCompute = !req.computeSpv.empty();
  const bool wantMeshGraphics = !req.meshSpv.empty() && !req.fragmentSpv.empty();
  const bool wantClassicGraphics = !req.vertexSpv.empty() && !req.fragmentSpv.empty();
  const bool wantGraphics = wantClassicGraphics || wantMeshGraphics;
  if (!wantCompute && !wantGraphics) {
    return Error::make(ErrorCode::InvalidWorkload,
                       "provide compute SPIR-V, vert+frag, or mesh+frag graphics SPIR-V");
  }

  VulkanSmokeResult out;
  auto &steps = out.steps;

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "csrun-spirv-run";
  app.apiVersion = VK_API_VERSION_1_2;

  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&ici, nullptr, &instance);
  if (r != VK_SUCCESS)
    return vkError("vkCreateInstance failed", r);
  steps.push_back("vkCreateInstance");

  uint32_t physCount = 0;
  r = vkEnumeratePhysicalDevices(instance, &physCount, nullptr);
  if (r != VK_SUCCESS || physCount == 0) {
    vkDestroyInstance(instance, nullptr);
    return vkError("vkEnumeratePhysicalDevices failed / no devices",
                   r == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : r);
  }
  std::vector<VkPhysicalDevice> phys(physCount);
  vkEnumeratePhysicalDevices(instance, &physCount, phys.data());
  steps.push_back("vkEnumeratePhysicalDevices (" + std::to_string(physCount) + " devices)");

  if (req.deviceIndex.has_value() && *req.deviceIndex >= physCount) {
    vkDestroyInstance(instance, nullptr);
    return Error::make(ErrorCode::DeviceNotFound, "device index out of range");
  }
  const uint32_t physIndex =
      vulkanPickPhysicalDevice(phys, req.deviceIndex, wantGraphics && !wantCompute);
  VkPhysicalDevice physical = phys[physIndex];
  const VulkanDeviceMeta meta = vulkanQueryDeviceMeta(physical, physIndex);
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physical, &props);
  out.deviceName = props.deviceName;
  steps.push_back("selected device[" + std::to_string(physIndex) + "]: " + meta.name + " [" +
                  meta.typeName + "] driver=" + meta.driverName +
                  (meta.hasPerfQuery ? " perf_query=yes" : " perf_query=no"));
  if (meta.isCpu || meta.driverId == VK_DRIVER_ID_MESA_LLVMPIPE) {
    steps.push_back("warning: software Vulkan device — AMD GPU not visible to this process "
                    "(check /dev/dri + force RADV ICD)");
  }
  if (const std::string tip = vulkanSuggestPerfQueryDevice(phys, physIndex); !tip.empty())
    steps.push_back(tip);

  uint32_t qCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qCount, nullptr);
  std::vector<VkQueueFamilyProperties> qprops(qCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qCount, qprops.data());

  uint32_t queueFamily = std::numeric_limits<uint32_t>::max();
  const VkQueueFlags need = wantCompute ? VK_QUEUE_COMPUTE_BIT : VK_QUEUE_GRAPHICS_BIT;
  for (uint32_t i = 0; i < qCount; ++i) {
    if (qprops[i].queueFlags & need) {
      queueFamily = i;
      break;
    }
  }
  if (queueFamily == std::numeric_limits<uint32_t>::max()) {
    vkDestroyInstance(instance, nullptr);
    return Error::make(ErrorCode::DeviceNotFound, "no suitable queue family");
  }

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = queueFamily;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  VkPhysicalDeviceFeatures supportedFeatures{};
  vkGetPhysicalDeviceFeatures(physical, &supportedFeatures);
  const bool pipeStatsSupported = supportedFeatures.pipelineStatisticsQuery == VK_TRUE;
  const VulkanPerfDeviceSetup perfSetup = vulkanPreparePerfQueryDevice(physical);
  std::vector<const char *> deviceExts;
  if (perfSetup.want)
    deviceExts.push_back(VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME);

  // Probe descriptor-indexing features (Vulkan 1.2 / EXT) for runtime arrays.
  VkPhysicalDeviceDescriptorIndexingFeatures indexingQuery{};
  indexingQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
  VkPhysicalDeviceFeatures2 query2{};
  query2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  query2.pNext = &indexingQuery;
  vkGetPhysicalDeviceFeatures2(physical, &query2);
  const bool wantIndexing = indexingQuery.runtimeDescriptorArray == VK_TRUE &&
                            indexingQuery.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;

  const bool wantGeometry = wantGraphics && !req.geometrySpv.empty();
  const bool wantTess = wantGraphics && (!req.tessControlSpv.empty() || !req.tessEvalSpv.empty());
  if (wantGeometry && !supportedFeatures.geometryShader) {
    vkDestroyInstance(instance, nullptr);
    Error e = Error::make(ErrorCode::UnsupportedOperation,
                          "device does not support geometryShader feature");
    e.backend = "vulkan";
    e.operation = "execute";
    return e;
  }
  if (wantTess && !supportedFeatures.tessellationShader) {
    vkDestroyInstance(instance, nullptr);
    Error e = Error::make(ErrorCode::UnsupportedOperation,
                          "device does not support tessellationShader feature");
    e.backend = "vulkan";
    e.operation = "execute";
    return e;
  }
  if (wantTess && (req.tessControlSpv.empty() || req.tessEvalSpv.empty())) {
    vkDestroyInstance(instance, nullptr);
    Error e = Error::make(ErrorCode::InvalidWorkload,
                          "tessellation requires both tesscontrol and tesseval SPIR-V");
    e.backend = "vulkan";
    e.operation = "execute";
    return e;
  }

  VkPhysicalDeviceFeatures enabledFeatures{};
  if (pipeStatsSupported)
    enabledFeatures.pipelineStatisticsQuery = VK_TRUE;
  if (supportedFeatures.shaderClipDistance)
    enabledFeatures.shaderClipDistance = VK_TRUE;
  if (wantGeometry)
    enabledFeatures.geometryShader = VK_TRUE;
  if (wantTess)
    enabledFeatures.tessellationShader = VK_TRUE;

  const bool wantMesh = wantGraphics && !req.meshSpv.empty();
  VkPhysicalDeviceMeshShaderFeaturesEXT meshQuery{};
  meshQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
  if (wantMesh) {
    VkPhysicalDeviceFeatures2 mq2{};
    mq2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    mq2.pNext = &meshQuery;
    vkGetPhysicalDeviceFeatures2(physical, &mq2);
    if (!meshQuery.meshShader) {
      vkDestroyInstance(instance, nullptr);
      Error e = Error::make(ErrorCode::UnsupportedOperation,
                            "device does not support meshShader (VK_EXT_mesh_shader)");
      e.backend = "vulkan";
      e.operation = "execute";
      return e;
    }
    if (!req.taskSpv.empty() && !meshQuery.taskShader) {
      vkDestroyInstance(instance, nullptr);
      Error e = Error::make(ErrorCode::UnsupportedOperation,
                            "device does not support taskShader (VK_EXT_mesh_shader)");
      e.backend = "vulkan";
      e.operation = "execute";
      return e;
    }
    // Check required device extensions are present.
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, exts.data());
    auto hasExt = [&](const char *name) {
      for (const auto &e : exts)
        if (std::strcmp(e.extensionName, name) == 0)
          return true;
      return false;
    };
    if (!hasExt(VK_EXT_MESH_SHADER_EXTENSION_NAME) ||
        !hasExt(VK_KHR_SPIRV_1_4_EXTENSION_NAME) ||
        !hasExt(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME)) {
      vkDestroyInstance(instance, nullptr);
      Error e = Error::make(ErrorCode::UnsupportedOperation,
                            "device missing mesh-shader related extensions");
      e.backend = "vulkan";
      e.operation = "execute";
      return e;
    }
    deviceExts.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    deviceExts.push_back(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
    deviceExts.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
  }

  VkPhysicalDeviceMeshShaderFeaturesEXT enabledMeshFeatures{};
  enabledMeshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
  if (wantMesh) {
    enabledMeshFeatures.meshShader = VK_TRUE;
    if (!req.taskSpv.empty())
      enabledMeshFeatures.taskShader = VK_TRUE;
  }

  VkPhysicalDeviceDescriptorIndexingFeatures enabledIndexing{};
  enabledIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
  if (wantIndexing) {
    enabledIndexing.runtimeDescriptorArray = VK_TRUE;
    enabledIndexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
  }

  // Probe buffer-device-address feature (GL_EXT_buffer_reference).
  VkPhysicalDeviceBufferDeviceAddressFeatures enabledBDAFeatures{};
  enabledBDAFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
  {
    VkPhysicalDeviceFeatures2 bdaProbe{};
    bdaProbe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    bdaProbe.pNext = &enabledBDAFeatures;
    vkGetPhysicalDeviceFeatures2(physical, &bdaProbe);
  }
  const bool wantBDA = enabledBDAFeatures.bufferDeviceAddress == VK_TRUE;
  if (wantBDA)
    enabledBDAFeatures.bufferDeviceAddress = VK_TRUE;

  // Probe inline-uniform-block feature (VK_EXT_inline_uniform_block).
  VkPhysicalDeviceInlineUniformBlockFeatures enabledInlineFeatures{};
  enabledInlineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES;
  {
    VkPhysicalDeviceFeatures2 inlineProbe{};
    inlineProbe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    inlineProbe.pNext = &enabledInlineFeatures;
    vkGetPhysicalDeviceFeatures2(physical, &inlineProbe);
  }
  const bool wantInlineUniform = enabledInlineFeatures.inlineUniformBlock == VK_TRUE;
  if (wantInlineUniform) {
    enabledInlineFeatures.inlineUniformBlock = VK_TRUE;
    enabledInlineFeatures.descriptorBindingInlineUniformBlockUpdateAfterBind = VK_TRUE;
    deviceExts.push_back(VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME);
  }

  VkPhysicalDevicePerformanceQueryFeaturesKHR enabledPerfFeatures = perfSetup.features;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.features = enabledFeatures;

  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
  dci.ppEnabledExtensionNames = deviceExts.empty() ? nullptr : deviceExts.data();
  // Chain: features2 → [bda] → [inline] → [mesh] → [indexing] → [perf]
  void *chain = nullptr;
  if (perfSetup.want) {
    enabledPerfFeatures.pNext = chain;
    chain = &enabledPerfFeatures;
  }
  if (wantIndexing) {
    enabledIndexing.pNext = chain;
    chain = &enabledIndexing;
  }
  if (wantMesh) {
    enabledMeshFeatures.pNext = chain;
    chain = &enabledMeshFeatures;
  }
  if (wantInlineUniform) {
    enabledInlineFeatures.pNext = chain;
    chain = &enabledInlineFeatures;
  }
  if (wantBDA) {
    enabledBDAFeatures.pNext = chain;
    chain = &enabledBDAFeatures;
  }
  if (chain || perfSetup.want || wantIndexing || wantMesh || wantBDA || wantInlineUniform) {
    features2.pNext = chain;
    dci.pNext = &features2;
    dci.pEnabledFeatures = nullptr;
  } else {
    dci.pEnabledFeatures = &enabledFeatures;
  }
  VkDevice device = VK_NULL_HANDLE;
  r = vkCreateDevice(physical, &dci, nullptr, &device);
  if (r != VK_SUCCESS) {
    vkDestroyInstance(instance, nullptr);
    return vkError("vkCreateDevice failed", r);
  }
  {
    std::string devNote = "vkCreateDevice";
    if (pipeStatsSupported)
      devNote += " (pipelineStatisticsQuery)";
    if (perfSetup.want)
      devNote += " (+ VK_KHR_performance_query)";
    if (wantIndexing)
      devNote += " (+ descriptor_indexing)";
    if (wantMesh)
      devNote += " (+ mesh_shader)";
    steps.push_back(devNote);
  }

  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(device, queueFamily, 0, &queue);

  auto cleanup = [&]() {
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
  };

  if (wantCompute) {
    out.mode = "compute_dispatch";
    auto words = loadWords(req.computeSpv);
    if (!words) {
      cleanup();
      return words.error();
    }
    steps.push_back("loaded compute SPIR-V: " + req.computeSpv.string());

    // Reflect first: UBO / storage-image / multi-set → generic path; else legacy 3-SSBO.
    {
      auto cRefl = reflectSpirvModule(words.value(), SpirvReflectStage::Fragment);
      if (!cRefl) {
        cleanup();
        return cRefl.error();
      }
      if (vulkanComputeNeedsGenericResources(cRefl.value())) {
        auto ran = vulkanRunComputeGeneric(physical, device, queue, queueFamily, words.value(), req,
                                           out);
        if (!ran) {
          cleanup();
          return ran.error();
        }
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        out.deviceName = props.deviceName;
        cleanup();
        return out;
      }
    }

    auto mod = createModule(device, words.value(), steps);
    if (!mod) {
      cleanup();
      return mod.error();
    }

    // Layout matches vector_add.comp (3 SSBOs + u32 push). Extra unused bindings are OK.
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 3;
    dslci.pBindings = bindings;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    r = vkCreateDescriptorSetLayout(device, &dslci, nullptr, &setLayout);
    if (r != VK_SUCCESS) {
      vkDestroyShaderModule(device, mod.value(), nullptr);
      cleanup();
      return vkError("vkCreateDescriptorSetLayout failed", r);
    }

    const bool isMatmul = req.dimM > 0 && req.dimN > 0 && req.dimK > 0;
    const uint32_t M = isMatmul ? static_cast<uint32_t>(req.dimM) : 0;
    const uint32_t N = isMatmul ? static_cast<uint32_t>(req.dimN) : 0;
    const uint32_t K = isMatmul ? static_cast<uint32_t>(req.dimK) : 0;
    const uint32_t n =
        isMatmul ? 0
                 : static_cast<uint32_t>(req.dimCount > 0 ? req.dimCount : 64);
    struct MatmulPush {
      uint32_t M, N, K;
    };
    const uint32_t pushBytes =
        isMatmul ? static_cast<uint32_t>(sizeof(MatmulPush)) : static_cast<uint32_t>(sizeof(uint32_t));

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = 0;
    push.size = pushBytes;

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    r = vkCreatePipelineLayout(device, &plci, nullptr, &layout);
    if (r != VK_SUCCESS) {
      vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
      vkDestroyShaderModule(device, mod.value(), nullptr);
      cleanup();
      return vkError("vkCreatePipelineLayout failed", r);
    }
    steps.push_back("vkCreatePipelineLayout");

    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = mod.value();
    cpci.stage.pName = "main";
    cpci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
      vkDestroyPipelineLayout(device, layout, nullptr);
      vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
      vkDestroyShaderModule(device, mod.value(), nullptr);
      cleanup();
      return vkError("vkCreateComputePipelines failed", r);
    }
    steps.push_back("vkCreateComputePipelines");

    const VkDeviceSize bytesA =
        sizeof(float) * (isMatmul ? static_cast<VkDeviceSize>(M) * K : n);
    const VkDeviceSize bytesB =
        sizeof(float) * (isMatmul ? static_cast<VkDeviceSize>(K) * N : n);
    const VkDeviceSize bytesC =
        sizeof(float) * (isMatmul ? static_cast<VkDeviceSize>(M) * N : n);

    std::vector<float> hostA(static_cast<size_t>(bytesA / sizeof(float)));
    std::vector<float> hostB(static_cast<size_t>(bytesB / sizeof(float)));
    std::vector<float> hostC(static_cast<size_t>(bytesC / sizeof(float)), 0.f);
    if (isMatmul) {
      for (uint32_t i = 0; i < M; ++i)
        for (uint32_t k = 0; k < K; ++k)
          hostA[static_cast<size_t>(i) * K + k] = 1.0f;
      // B[k,j] = 1/K → C[i,j] = 1 for all entries (easy host check).
      for (uint32_t k = 0; k < K; ++k)
        for (uint32_t j = 0; j < N; ++j)
          hostB[static_cast<size_t>(k) * N + j] = 1.0f / static_cast<float>(K);
    } else {
      for (uint32_t i = 0; i < n; ++i) {
        hostA[i] = static_cast<float>(i);
        hostB[i] = static_cast<float>(i);
      }
    }

    auto makeBufSized = [&](VkBuffer &buf, VkDeviceMemory &mem, VkDeviceSize sz) -> Expected<void> {
      VkBufferCreateInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bi.size = sz;
      bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      VkResult br = vkCreateBuffer(device, &bi, nullptr, &buf);
      if (br != VK_SUCCESS)
        return vkError("vkCreateBuffer failed", br);
      VkMemoryRequirements mreq{};
      vkGetBufferMemoryRequirements(device, buf, &mreq);
      const uint32_t mt = findMemoryType(
          physical, mreq.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (mt == std::numeric_limits<uint32_t>::max())
        return Error::make(ErrorCode::ResourceAllocationFailed, "no host memory for SSBO");
      VkMemoryAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ai.allocationSize = mreq.size;
      ai.memoryTypeIndex = mt;
      br = vkAllocateMemory(device, &ai, nullptr, &mem);
      if (br != VK_SUCCESS)
        return vkError("vkAllocateMemory failed", br);
      vkBindBufferMemory(device, buf, mem, 0);
      return Expected<void>{};
    };

    VkBuffer bufA = VK_NULL_HANDLE, bufB = VK_NULL_HANDLE, bufC = VK_NULL_HANDLE;
    VkDeviceMemory memA = VK_NULL_HANDLE, memB = VK_NULL_HANDLE, memC = VK_NULL_HANDLE;
    VulkanPerfQuerySession perf;
    double h2dMs = 0.0;
    double d2hMs = 0.0;
    auto failCompute = [&](const Error &e) -> Expected<VulkanSmokeResult> {
      vulkanDestroyPerfQuerySession(device, perf);
      if (bufA)
        vkDestroyBuffer(device, bufA, nullptr);
      if (bufB)
        vkDestroyBuffer(device, bufB, nullptr);
      if (bufC)
        vkDestroyBuffer(device, bufC, nullptr);
      if (memA)
        vkFreeMemory(device, memA, nullptr);
      if (memB)
        vkFreeMemory(device, memB, nullptr);
      if (memC)
        vkFreeMemory(device, memC, nullptr);
      vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyPipelineLayout(device, layout, nullptr);
      vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
      vkDestroyShaderModule(device, mod.value(), nullptr);
      cleanup();
      return e;
    };

    if (auto a = makeBufSized(bufA, memA, bytesA); !a)
      return failCompute(a.error());
    if (auto b = makeBufSized(bufB, memB, bytesB); !b)
      return failCompute(b.error());
    if (auto c = makeBufSized(bufC, memC, bytesC); !c)
      return failCompute(c.error());

    {
      const auto t0 = std::chrono::steady_clock::now();
      void *p = nullptr;
      vkMapMemory(device, memA, 0, bytesA, 0, &p);
      std::memcpy(p, hostA.data(), static_cast<size_t>(bytesA));
      vkUnmapMemory(device, memA);
      vkMapMemory(device, memB, 0, bytesB, 0, &p);
      std::memcpy(p, hostB.data(), static_cast<size_t>(bytesB));
      vkUnmapMemory(device, memB);
      vkMapMemory(device, memC, 0, bytesC, 0, &p);
      std::memcpy(p, hostC.data(), static_cast<size_t>(bytesC));
      vkUnmapMemory(device, memC);
      const auto t1 = std::chrono::steady_clock::now();
      h2dMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
    if (isMatmul)
      steps.push_back("allocated matmul buffers M=" + std::to_string(M) + " N=" +
                      std::to_string(N) + " K=" + std::to_string(K) +
                      " h2d_ms=" + std::to_string(h2dMs));
    else
      steps.push_back("allocated 3 storage buffers (n=" + std::to_string(n) +
                      ") h2d_ms=" + std::to_string(h2dMs));

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 3;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    r = vkCreateDescriptorPool(device, &dpci, nullptr, &descPool);
    if (r != VK_SUCCESS)
      return failCompute(vkError("vkCreateDescriptorPool failed", r));

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    r = vkAllocateDescriptorSets(device, &dsai, &set);
    if (r != VK_SUCCESS) {
      vkDestroyDescriptorPool(device, descPool, nullptr);
      return failCompute(vkError("vkAllocateDescriptorSets failed", r));
    }

    VkDescriptorBufferInfo bis[3]{};
    VkBuffer bufs[3] = {bufA, bufB, bufC};
    const VkDeviceSize ranges[3] = {bytesA, bytesB, bytesC};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
      bis[i].buffer = bufs[i];
      bis[i].range = ranges[i];
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &bis[i];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    VkCommandPoolCreateInfo poolCi{};
    poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCi.queueFamilyIndex = queueFamily;
    poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    r = vkCreateCommandPool(device, &poolCi, nullptr, &cmdPool);
    if (r != VK_SUCCESS) {
      vkDestroyDescriptorPool(device, descPool, nullptr);
      return failCompute(vkError("vkCreateCommandPool failed", r));
    }

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cbai, &cmd);

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qCount, qprops.data());
    const bool timestampsSupported =
        props.limits.timestampPeriod > 0.0f && queueFamily < qCount &&
        qprops[queueFamily].timestampValidBits > 0;
    const float timestampPeriodNs = props.limits.timestampPeriod;

    VkQueryPool queryPool = VK_NULL_HANDLE;
    if (timestampsSupported) {
      VkQueryPoolCreateInfo qpci{};
      qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qpci.queryCount = PipelineStats::kComputeTimestampCount;
      if (vkCreateQueryPool(device, &qpci, nullptr, &queryPool) != VK_SUCCESS)
        queryPool = VK_NULL_HANDLE;
      else
        steps.push_back("compute stage timestamps enabled (" +
                        std::to_string(PipelineStats::kComputeTimestampCount) +
                        " markers: TOP/CS/BOTTOM)");
    }

    const VkQueryPipelineStatisticFlags statFlags = computePipelineStatFlags();
    VkQueryPool statsPool = VK_NULL_HANDLE;
    if (pipeStatsSupported) {
      VkQueryPoolCreateInfo spci{};
      spci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      spci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
      spci.queryCount = 1;
      spci.pipelineStatistics = statFlags;
      if (vkCreateQueryPool(device, &spci, nullptr, &statsPool) != VK_SUCCESS)
        statsPool = VK_NULL_HANDLE;
      else
        steps.push_back("compute pipeline statistics enabled");
    }

    perf = vulkanCreatePerfQuerySession(instance, physical, device, queueFamily, steps,
                                        req.vendorCounters);
    if (!perf.available) {
      perf.note = vulkanPerfQueryHint(meta);
      if (!steps.empty() && steps.back().rfind("vendor HW counters:", 0) == 0)
        steps.back() = "vendor HW counters: " + perf.note;
    }

    const int warmupIters = std::max(0, req.warmup);
    const int timedIters = std::max(1, req.iterations);
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(timedIters));
    std::vector<PipelineStats> counterSamples;
    counterSamples.reserve(static_cast<size_t>(timedIters));
    TimingSource timingSource = TimingSource::CpuWall;

    auto destroyComputeQueries = [&]() {
      vulkanDestroyPerfQuerySession(device, perf);
      if (statsPool)
        vkDestroyQueryPool(device, statsPool, nullptr);
      if (queryPool)
        vkDestroyQueryPool(device, queryPool, nullptr);
      statsPool = VK_NULL_HANDLE;
      queryPool = VK_NULL_HANDLE;
    };

    auto recordAndSubmit = [&](bool withTimestamps, bool withStats) -> Expected<double> {
      PipelineStats iterCounters;
      const bool withPerf = withStats && perf.available;
      const uint32_t submitPasses = (withPerf && perf.passCount > 1) ? perf.passCount : 1;
      double gpuMs = 0.0;

      for (uint32_t pass = 0; pass < submitPasses; ++pass) {
        const bool lastPass = (pass + 1 == submitPasses);
        const auto tRecord0 = std::chrono::steady_clock::now();
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        if (lastPass && withStats && statsPool)
          vkCmdResetQueryPool(cmd, statsPool, 0, 1);
        if (withPerf && pass == 0)
          vulkanPerfCmdReset(cmd, perf);
        if (lastPass && withTimestamps && queryPool) {
          vkCmdResetQueryPool(cmd, queryPool, 0, PipelineStats::kComputeTimestampCount);
          vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
        }
        if (lastPass && withStats && statsPool)
          vkCmdBeginQuery(cmd, statsPool, 0, 0);
        if (withPerf)
          vulkanPerfCmdBegin(cmd, perf);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
                                nullptr);
        if (isMatmul) {
          MatmulPush mp{M, N, K};
          vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mp), &mp);
          const uint32_t gx = (N + 15u) / 16u;
          const uint32_t gy = (M + 15u) / 16u;
          vkCmdDispatch(cmd, gx, gy, 1);
        } else {
          vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &n);
          const uint32_t groups = (n + 255u) / 256u;
          vkCmdDispatch(cmd, std::max(1u, groups), 1, 1);
        }
        if (lastPass && withTimestamps && queryPool)
          vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queryPool, 1);
        if (withPerf)
          vulkanPerfCmdEnd(cmd, perf);
        if (lastPass && withStats && statsPool)
          vkCmdEndQuery(cmd, statsPool, 0);
        if (lastPass && withTimestamps && queryPool)
          vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 2);
        vkEndCommandBuffer(cmd);
        const auto tRecord1 = std::chrono::steady_clock::now();
        if (pass == 0)
          iterCounters.hostRecordMs =
              std::chrono::duration<double, std::milli>(tRecord1 - tRecord0).count();

        const auto tSubmit0 = std::chrono::steady_clock::now();
        VkPerformanceQuerySubmitInfoKHR perfSubmit{};
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        if (withPerf && perf.passCount > 1) {
          perfSubmit.sType = VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR;
          perfSubmit.counterPassIndex = pass;
          submit.pNext = &perfSubmit;
        }
        VkResult sr = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        if (sr != VK_SUCCESS)
          return vkError("vkQueueSubmit failed", sr);
        const auto tSubmitEnd = std::chrono::steady_clock::now();
        iterCounters.hostSubmitMs +=
            std::chrono::duration<double, std::milli>(tSubmitEnd - tSubmit0).count();
      }

      const auto tWait0 = std::chrono::steady_clock::now();
      vkQueueWaitIdle(queue);
      const auto tWait1 = std::chrono::steady_clock::now();
      iterCounters.hostQueueWaitMs =
          std::chrono::duration<double, std::milli>(tWait1 - tWait0).count();
      iterCounters.dispatchCount = 1;

      if (withStats && statsPool) {
        uint64_t csInv = 0;
        if (vulkanGetQueryResultsU64(device, statsPool, 0, 1, &csInv, sizeof(uint64_t))) {
          iterCounters.available = true;
          iterCounters.csInvocations = csInv;
        }
      }
      if (withPerf) {
        std::string vNote;
        if (vulkanPerfQueryResults(device, perf, iterCounters.vendor, vNote)) {
          iterCounters.vendorApi = "VK_KHR_performance_query";
          iterCounters.vendorNote = vNote;
        } else if (!vNote.empty()) {
          iterCounters.vendorApi = "VK_KHR_performance_query";
          iterCounters.vendorNote = vNote;
        }
      }
      if (withStats)
        counterSamples.push_back(iterCounters);

      if (withTimestamps && queryPool) {
        uint64_t stamps[PipelineStats::kComputeTimestampCount] = {};
        if (vulkanGetQueryResultsU64(device, queryPool, 0, PipelineStats::kComputeTimestampCount,
                                     stamps, sizeof(uint64_t)) &&
            PipelineStats::fillComputeGpuStages(iterCounters.gpuStages, stamps,
                                                PipelineStats::kComputeTimestampCount,
                                                timestampPeriodNs)) {
          gpuMs = iterCounters.gpuStages.totalMs;
          iterCounters.gpuElapsedMs = gpuMs;
          iterCounters.gpuTimestampAvailable = true;
          if (withStats && !counterSamples.empty())
            counterSamples.back() = iterCounters;
          return gpuMs;
        }
        if (stamps[PipelineStats::kComputeTimestampCount - 1] >= stamps[0] &&
            stamps[PipelineStats::kComputeTimestampCount - 1] != 0) {
          gpuMs = (static_cast<double>(stamps[PipelineStats::kComputeTimestampCount - 1] -
                                       stamps[0]) *
                   timestampPeriodNs) /
                  1.0e6;
          iterCounters.gpuElapsedMs = gpuMs;
          iterCounters.gpuTimestampAvailable = true;
          if (withStats && !counterSamples.empty())
            counterSamples.back() = iterCounters;
          return gpuMs;
        }
        gpuMs = iterCounters.hostSubmitMs + iterCounters.hostQueueWaitMs;
        return gpuMs;
      }
      return iterCounters.hostSubmitMs;
    };

    for (int i = 0; i < warmupIters; ++i) {
      auto w = recordAndSubmit(false, false);
      if (!w) {
        destroyComputeQueries();
        vkDestroyCommandPool(device, cmdPool, nullptr);
        vkDestroyDescriptorPool(device, descPool, nullptr);
        return failCompute(w.error());
      }
    }

    for (int i = 0; i < timedIters; ++i) {
      const bool useTs = timestampsSupported && queryPool != VK_NULL_HANDLE;
      auto ms = recordAndSubmit(useTs, true);
      if (!ms) {
        destroyComputeQueries();
        vkDestroyCommandPool(device, cmdPool, nullptr);
        vkDestroyDescriptorPool(device, descPool, nullptr);
        return failCompute(ms.error());
      }
      samples.push_back(ms.value());
      timingSource = useTs ? TimingSource::GpuTimestamp : TimingSource::CpuWall;
    }

    out.samplesMs = std::move(samples);
    out.timingSource = timingSource;
    out.counters = PipelineStats::medianOf(counterSamples);
    out.counters.hostH2dMs = h2dMs;
    if (out.counters.vendorNote.empty() && !perf.note.empty())
      out.counters.vendorNote = perf.note;

    // D2H + optional host check
    {
      const auto t0 = std::chrono::steady_clock::now();
      void *p = nullptr;
      vkMapMemory(device, memC, 0, bytesC, 0, &p);
      std::memcpy(hostC.data(), p, static_cast<size_t>(bytesC));
      vkUnmapMemory(device, memC);
      const auto t1 = std::chrono::steady_clock::now();
      d2hMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
      out.counters.hostD2hMs = d2hMs;
      steps.push_back("d2h_ms=" + std::to_string(d2hMs));
    }
    if (req.validate) {
      bool okCheck = true;
      if (isMatmul) {
        for (uint32_t i = 0; i < M && okCheck; ++i) {
          for (uint32_t j = 0; j < N && okCheck; ++j) {
            float expect = 0.f;
            for (uint32_t k = 0; k < K; ++k)
              expect += hostA[static_cast<size_t>(i) * K + k] *
                        hostB[static_cast<size_t>(k) * N + j];
            const float got = hostC[static_cast<size_t>(i) * N + j];
            if (std::abs(got - expect) > static_cast<float>(req.absTol) * (1.f + std::abs(expect)))
              okCheck = false;
          }
        }
      } else {
        for (uint32_t i = 0; i < n; ++i) {
          const float expect = hostA[i] + hostB[i];
          if (std::abs(hostC[i] - expect) > static_cast<float>(req.absTol) * (1.f + std::abs(expect))) {
            okCheck = false;
            break;
          }
        }
      }
      out.computeChecked = okCheck;
      out.ok = okCheck;
      steps.push_back(okCheck ? "host validation PASS" : "host validation FAIL");
    }

    out.hostMs = 0.0;
    for (double s : out.samplesMs)
      out.hostMs += s;
    out.executed = true;
    if (!req.validate)
      out.ok = true;
    steps.push_back("vkCmdDispatch timed (" + std::to_string(timedIters) + " iters, " +
                    std::string(toString(timingSource)) + ")");
    if (out.counters.available)
      steps.push_back(out.counters.summaryLine() + " (median over timed iters)");
    if (!out.counters.vendor.empty() || !out.counters.vendorNote.empty()) {
      const std::string report = out.counters.checklistReport();
      const auto nl = report.find('\n');
      steps.push_back(nl == std::string::npos ? report : report.substr(0, nl));
    }

    destroyComputeQueries();
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyBuffer(device, bufA, nullptr);
    vkDestroyBuffer(device, bufB, nullptr);
    vkDestroyBuffer(device, bufC, nullptr);
    vkFreeMemory(device, memA, nullptr);
    vkFreeMemory(device, memB, nullptr);
    vkFreeMemory(device, memC, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    vkDestroyShaderModule(device, mod.value(), nullptr);
    steps.push_back("cleanup compute objects");
  } else {
    // Windowed path: real Vulkan WSI (surface + swapchain + present).
    if (req.show) {
      VulkanWsiRequest wsi;
      wsi.vertexSpv = req.vertexSpv;
      wsi.fragmentSpv = req.fragmentSpv;
      wsi.deviceIndex = req.deviceIndex;
      wsi.presentMs = req.showMs;
      wsi.width = req.windowWidth;
      wsi.height = req.windowHeight;
      wsi.title = "csrun — Vulkan WSI";
      auto presented = vulkanPresentGraphics(wsi);
      if (!presented)
        return presented.error();
      out.mode = "graphics_wsi_present";
      out.deviceName = presented.value().deviceName;
      out.steps = presented.value().steps;
      out.hostMs = presented.value().hostMs;
      out.framesPresented = presented.value().framesPresented;
      out.executed = presented.value().framesPresented > 0;
      out.ok = presented.value().ok;
      out.timingSource = TimingSource::CpuWall;
      if (out.framesPresented > 0 && out.hostMs > 0.0)
        out.samplesMs.push_back(out.hostMs / static_cast<double>(out.framesPresented));
      else if (out.hostMs > 0.0)
        out.samplesMs.push_back(out.hostMs);
      out.counters = presented.value().counters;
      logInfo("runtime", "Vulkan SPIR-V " + out.mode + (out.ok ? " OK" : " FAIL") + " on " +
                             out.deviceName + " (executed)");
      return out;
    }

    out.mode = "graphics_offscreen";
    const bool useMeshPipeline = !req.meshSpv.empty();
    std::optional<std::vector<uint32_t>> vWords;
    if (!useMeshPipeline) {
      auto vw = loadWords(req.vertexSpv);
      if (!vw) {
        cleanup();
        return vw.error();
      }
      vWords = std::move(vw.value());
      steps.push_back("loaded vertex SPIR-V: " + req.vertexSpv.string());
    }
    auto fWords = loadWords(req.fragmentSpv);
    if (!fWords) {
      cleanup();
      return fWords.error();
    }
    steps.push_back("loaded fragment SPIR-V: " + req.fragmentSpv.string());

    std::optional<std::vector<uint32_t>> gWords;
    std::optional<std::vector<uint32_t>> tcWords;
    std::optional<std::vector<uint32_t>> teWords;
    std::optional<SpirvModuleReflection> gRefl;
    std::optional<SpirvModuleReflection> tcRefl;
    std::optional<SpirvModuleReflection> teRefl;
    if (!req.geometrySpv.empty()) {
      auto gw = loadWords(req.geometrySpv);
      if (!gw) {
        cleanup();
        return gw.error();
      }
      gWords = std::move(gw.value());
      steps.push_back("loaded geometry SPIR-V: " + req.geometrySpv.string());
      auto gr = reflectSpirvModule(*gWords, SpirvReflectStage::Geometry);
      if (!gr) {
        cleanup();
        return gr.error();
      }
      gRefl = std::move(gr.value());
    }
    if (!req.tessControlSpv.empty()) {
      auto tw = loadWords(req.tessControlSpv);
      if (!tw) {
        cleanup();
        return tw.error();
      }
      tcWords = std::move(tw.value());
      steps.push_back("loaded tesscontrol SPIR-V: " + req.tessControlSpv.string());
      auto tr = reflectSpirvModule(*tcWords, SpirvReflectStage::TessControl);
      if (!tr) {
        cleanup();
        return tr.error();
      }
      tcRefl = std::move(tr.value());
    }
    if (!req.tessEvalSpv.empty()) {
      auto tw = loadWords(req.tessEvalSpv);
      if (!tw) {
        cleanup();
        return tw.error();
      }
      teWords = std::move(tw.value());
      steps.push_back("loaded tesseval SPIR-V: " + req.tessEvalSpv.string());
      auto tr = reflectSpirvModule(*teWords, SpirvReflectStage::TessEvaluation);
      if (!tr) {
        cleanup();
        return tr.error();
      }
      teRefl = std::move(tr.value());
    }

    std::optional<std::vector<uint32_t>> meshWords;
    std::optional<std::vector<uint32_t>> taskWords;
    std::optional<SpirvModuleReflection> meshRefl;
    std::optional<SpirvModuleReflection> taskRefl;
    if (!req.meshSpv.empty()) {
      auto mw = loadWords(req.meshSpv);
      if (!mw) {
        cleanup();
        return mw.error();
      }
      meshWords = std::move(mw.value());
      steps.push_back("loaded mesh SPIR-V: " + req.meshSpv.string());
      auto mr = reflectSpirvModule(*meshWords, SpirvReflectStage::Mesh);
      if (!mr) {
        cleanup();
        return mr.error();
      }
      meshRefl = std::move(mr.value());
    }
    if (!req.taskSpv.empty()) {
      auto tw = loadWords(req.taskSpv);
      if (!tw) {
        cleanup();
        return tw.error();
      }
      taskWords = std::move(tw.value());
      steps.push_back("loaded task SPIR-V: " + req.taskSpv.string());
      auto tr = reflectSpirvModule(*taskWords, SpirvReflectStage::Task);
      if (!tr) {
        cleanup();
        return tr.error();
      }
      taskRefl = std::move(tr.value());
    }

    SpirvModuleReflection vReflValue{};
    if (vWords) {
      auto vRefl = reflectSpirvModule(*vWords, SpirvReflectStage::Vertex);
      if (!vRefl) {
        cleanup();
        return vRefl.error();
      }
      vReflValue = std::move(vRefl.value());
    }
    auto fRefl = reflectSpirvModule(fWords.value(), SpirvReflectStage::Fragment);
    if (!fRefl) {
      cleanup();
      return fRefl.error();
    }
    SpirvExtraStageReflections extra{};
    if (gRefl)
      extra.geometry = &*gRefl;
    if (tcRefl)
      extra.tessControl = &*tcRefl;
    if (teRefl)
      extra.tessEvaluation = &*teRefl;
    if (meshRefl)
      extra.mesh = &*meshRefl;
    if (taskRefl)
      extra.task = &*taskRefl;
    const SpirvModuleReflection gfxRefl =
        mergeSpirvReflections(vReflValue, fRefl.value(), extra);
    auto gfxRes = vulkanCreateGraphicsResources(physical, device, queue, queueFamily, gfxRefl, steps);
    if (!gfxRes) {
      cleanup();
      return gfxRes.error();
    }
    VulkanGraphicsResources resources = std::move(gfxRes.value());

    VkShaderModule vMod = VK_NULL_HANDLE;
    if (vWords) {
      auto vm = createModule(device, *vWords, steps);
      if (!vm) {
        vulkanDestroyGraphicsResources(device, resources);
        cleanup();
        return vm.error();
      }
      vMod = vm.value();
    }
    auto fMod = createModule(device, fWords.value(), steps);
    if (!fMod) {
      if (vMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, vMod, nullptr);
      vulkanDestroyGraphicsResources(device, resources);
      cleanup();
      return fMod.error();
    }
    VkShaderModule gMod = VK_NULL_HANDLE;
    VkShaderModule tcMod = VK_NULL_HANDLE;
    VkShaderModule teMod = VK_NULL_HANDLE;
    VkShaderModule meshMod = VK_NULL_HANDLE;
    VkShaderModule taskMod = VK_NULL_HANDLE;
    auto failMods = [&](const Error &e) -> Error {
      if (taskMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, taskMod, nullptr);
      if (meshMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, meshMod, nullptr);
      if (teMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, teMod, nullptr);
      if (tcMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, tcMod, nullptr);
      if (gMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, gMod, nullptr);
      vkDestroyShaderModule(device, fMod.value(), nullptr);
      if (vMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, vMod, nullptr);
      vulkanDestroyGraphicsResources(device, resources);
      cleanup();
      return e;
    };
    if (gWords) {
      auto gm = createModule(device, *gWords, steps);
      if (!gm)
        return failMods(gm.error());
      gMod = gm.value();
    }
    if (tcWords) {
      auto tm = createModule(device, *tcWords, steps);
      if (!tm)
        return failMods(tm.error());
      tcMod = tm.value();
    }
    if (teWords) {
      auto tm = createModule(device, *teWords, steps);
      if (!tm)
        return failMods(tm.error());
      teMod = tm.value();
    }
    if (meshWords) {
      auto mm = createModule(device, *meshWords, steps);
      if (!mm)
        return failMods(mm.error());
      meshMod = mm.value();
    }
    if (taskWords) {
      auto tm = createModule(device, *taskWords, steps);
      if (!tm)
        return failMods(tm.error());
      taskMod = tm.value();
    }
    auto destroyShaderMods = [&]() {
      if (taskMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, taskMod, nullptr);
      if (meshMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, meshMod, nullptr);
      if (teMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, teMod, nullptr);
      if (tcMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, tcMod, nullptr);
      if (gMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, gMod, nullptr);
      vkDestroyShaderModule(device, fMod.value(), nullptr);
      if (vMod != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, vMod, nullptr);
    };

    VkPipelineLayout layout = resources.pipelineLayout;

    // `subpassInput` reads a render pass attachment, and an attachment cannot be written
    // and read as an input in the same subpass. So a shader with subpass inputs gets
    // `inputCount` primed attachments plus its own outputs: subpass 0 only clears the
    // inputs, subpass 1 renders with the pipeline. Shaders without subpass inputs keep the
    // single-attachment, single-subpass path.
    //
    // `outputCount` comes from the fragment shader's declared `Location`s. A shader writing
    // several locations needs one attachment each: the render pass and the blend state must
    // cover every location, because a fragment write to a location the pipeline does not
    // bind as a colour attachment is undefined, not merely dropped.
    const uint32_t inputCount = inputAttachmentCount(resources.inputAttachments);
    const uint32_t outputCount = resources.reflection.colorOutputCount == 0
                                     ? 1u
                                     : resources.reflection.colorOutputCount;
    if (outputCount > kMaxColorOutputs) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "fragment shader declares " + std::to_string(outputCount) +
                             " colour outputs; this smoke pass supports at most " +
                             std::to_string(kMaxColorOutputs));
    }
    const uint32_t outputAttachment = inputCount;
    const uint32_t attachmentCount = inputCount + outputCount;
    const uint32_t subpassIndex = inputCount > 0 ? 1u : 0u;
    if (inputCount > 0) {
      // Print the priming values: the frame the check sees is a function of these, so the
      // run stays auditable about what data the subpass load actually read.
      std::string primed;
      for (uint32_t i = 0; i < inputCount; ++i) {
        if (i)
          primed += ",";
        primed += std::to_string(inputAttachmentPrimeValue(i, inputCount));
      }
      steps.push_back("subpass inputs: " + std::to_string(inputCount) +
                      " primed attachment(s) [grey=" + primed + "] + " +
                      std::to_string(outputCount) + " output attachment(s)");
    } else if (outputCount > 1) {
      steps.push_back("MRT: " + std::to_string(outputCount) +
                      " colour targets from fragment output locations");
    }

    std::vector<VkAttachmentDescription> attachments(attachmentCount);
    for (VkAttachmentDescription &a : attachments) {
      a.format = VK_FORMAT_R8G8B8A8_UNORM;
      a.samples = VK_SAMPLE_COUNT_1_BIT;
      a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    std::vector<VkAttachmentReference> inputRefs(inputCount);
    for (uint32_t i = 0; i < inputCount; ++i) {
      // Inputs keep COLOR_ATTACHMENT_OPTIMAL: the descriptor image layout and the subpass
      // 1 reference both name it, and nothing transitions it away before the render pass ends.
      attachments[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      inputRefs[i].attachment = i;
      inputRefs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    // One reference per declared output location, so an MRT shader binds every location it
    // writes instead of leaving the extras undefined.
    std::vector<VkAttachmentReference> outputRefs(outputCount);
    for (uint32_t i = 0; i < outputCount; ++i) {
      outputRefs[i].attachment = outputAttachment + i;
      outputRefs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    std::vector<VkSubpassDescription> subpasses(inputCount > 0 ? 2 : 1);
    subpasses[subpassIndex].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[subpassIndex].colorAttachmentCount = outputCount;
    subpasses[subpassIndex].pColorAttachments = outputRefs.data();
    if (inputCount > 0) {
      // Priming subpass: colour attachments only so vkCmdBeginRenderPass clears them.
      // No pipeline is created for it and the command buffer draws nothing in it.
      subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpasses[0].colorAttachmentCount = inputCount;
      subpasses[0].pColorAttachments = inputRefs.data();
      subpasses[1].inputAttachmentCount = inputCount;
      subpasses[1].pInputAttachments = inputRefs.data();
    }

    std::vector<VkSubpassDependency> deps(inputCount > 0 ? 2 : 1);
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (inputCount > 0) {
      // The clear that primes the inputs must be visible to the fragment shader's load.
      deps[1].srcSubpass = 0;
      deps[1].dstSubpass = 1;
      deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    }

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = attachmentCount;
    rpci.pAttachments = attachments.data();
    rpci.subpassCount = static_cast<uint32_t>(subpasses.size());
    rpci.pSubpasses = subpasses.data();
    rpci.dependencyCount = static_cast<uint32_t>(deps.size());
    rpci.pDependencies = deps.data();
    VkRenderPass renderPass = VK_NULL_HANDLE;
    r = vkCreateRenderPass(device, &rpci, nullptr, &renderPass);
    if (r != VK_SUCCESS) {
      destroyShaderMods();
      vulkanDestroyGraphicsResources(device, resources);
      cleanup();
      return vkError("vkCreateRenderPass failed", r);
    }
    steps.push_back("vkCreateRenderPass");

    VkPipelineShaderStageCreateInfo stagesInfo[6]{};
    uint32_t stageCount = 0;
    auto addStage = [&](VkShaderStageFlagBits stage, VkShaderModule mod) {
      stagesInfo[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stagesInfo[stageCount].stage = stage;
      stagesInfo[stageCount].module = mod;
      stagesInfo[stageCount].pName = "main";
      ++stageCount;
    };
    if (useMeshPipeline) {
      if (taskMod != VK_NULL_HANDLE)
        addStage(VK_SHADER_STAGE_TASK_BIT_EXT, taskMod);
      addStage(VK_SHADER_STAGE_MESH_BIT_EXT, meshMod);
      addStage(VK_SHADER_STAGE_FRAGMENT_BIT, fMod.value());
      steps.push_back("mesh/task stages enabled in graphics pipeline");
    } else {
      addStage(VK_SHADER_STAGE_VERTEX_BIT, vMod);
      if (tcMod != VK_NULL_HANDLE)
        addStage(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, tcMod);
      if (teMod != VK_NULL_HANDLE)
        addStage(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, teMod);
      if (gMod != VK_NULL_HANDLE)
        addStage(VK_SHADER_STAGE_GEOMETRY_BIT, gMod);
      addStage(VK_SHADER_STAGE_FRAGMENT_BIT, fMod.value());
      if (tcMod != VK_NULL_HANDLE || teMod != VK_NULL_HANDLE)
        steps.push_back("tessellation stages enabled in graphics pipeline");
      if (gMod != VK_NULL_HANDLE)
        steps.push_back("geometry stage enabled in graphics pipeline");
    }

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (resources.hasVertexAttrs) {
      vi.vertexBindingDescriptionCount = static_cast<uint32_t>(resources.vertexBindings.size());
      vi.pVertexBindingDescriptions = resources.vertexBindings.data();
      vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(resources.vertexAttrs.size());
      vi.pVertexAttributeDescriptions = resources.vertexAttrs.data();
    }

    const bool useTess = (tcMod != VK_NULL_HANDLE && teMod != VK_NULL_HANDLE);
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = useTess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineTessellationStateCreateInfo tess{};
    tess.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tess.patchControlPoints = 3;

    VkViewport viewport{};
    const uint32_t extentW = std::max(1u, req.width);
    const uint32_t extentH = std::max(1u, req.height);
    viewport.width = static_cast<float>(extentW);
    viewport.height = static_cast<float>(extentH);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {extentW, extentH};

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // A vector, not a single state: the count has to match the subpass colour attachment
    // count or the pipeline and render pass disagree about how many targets exist.
    std::vector<VkPipelineColorBlendAttachmentState> blendAtts(outputCount);
    for (VkPipelineColorBlendAttachmentState &b : blendAtts) {
      b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = outputCount;
    cb.pAttachments = blendAtts.data();

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = stageCount;
    gpci.pStages = stagesInfo;
    gpci.pVertexInputState = useMeshPipeline ? nullptr : &vi;
    gpci.pInputAssemblyState = useMeshPipeline ? nullptr : &ia;
    gpci.pTessellationState = (!useMeshPipeline && useTess) ? &tess : nullptr;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &cb;
    gpci.layout = layout;
    gpci.renderPass = renderPass;
    gpci.subpass = subpassIndex;

    VkPipeline pipeline = VK_NULL_HANDLE;
    r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
      vkDestroyRenderPass(device, renderPass, nullptr);
      vulkanDestroyGraphicsResources(device, resources);
      destroyShaderMods();
      cleanup();
      return vkError("vkCreateGraphicsPipelines failed", r);
    }
    steps.push_back("vkCreateGraphicsPipelines");

    PFN_vkCmdDrawMeshTasksEXT cmdDrawMeshTasksEXT = nullptr;
    if (useMeshPipeline) {
      cmdDrawMeshTasksEXT = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(
          vkGetDeviceProcAddr(device, "vkCmdDrawMeshTasksEXT"));
      if (!cmdDrawMeshTasksEXT) {
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vulkanDestroyGraphicsResources(device, resources);
        destroyShaderMods();
        cleanup();
        return Error::make(ErrorCode::UnsupportedOperation,
                           "vkCmdDrawMeshTasksEXT not available");
      }
    }

    // Indirect draw command buffer: the smoke path always uses vkCmdDrawIndirect with a
    // pre-filled VkDrawIndirectCommand so the indirect draw path is exercised.
    VkBuffer indirectBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indirectMem = VK_NULL_HANDLE;
    if (!useMeshPipeline) {
      VkDrawIndirectCommand drawCmd{};
      drawCmd.vertexCount = resources.hasVertexAttrs ? resources.vertexCount : 3;
      drawCmd.instanceCount = 1;
      drawCmd.firstVertex = 0;
      drawCmd.firstInstance = 0;
      VkBufferCreateInfo indCi{};
      indCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      indCi.size = sizeof(VkDrawIndirectCommand);
      indCi.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
      indCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      r = vkCreateBuffer(device, &indCi, nullptr, &indirectBuffer);
      if (r != VK_SUCCESS) {
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vulkanDestroyGraphicsResources(device, resources);
        destroyShaderMods();
        cleanup();
        return vkError("vkCreateBuffer (indirect) failed", r);
      }
      VkMemoryRequirements indReq{};
      vkGetBufferMemoryRequirements(device, indirectBuffer, &indReq);
      const uint32_t indMemType = findMemoryType(
          physical, indReq.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (indMemType == std::numeric_limits<uint32_t>::max()) {
        vkDestroyBuffer(device, indirectBuffer, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vulkanDestroyGraphicsResources(device, resources);
        destroyShaderMods();
        cleanup();
        return Error::make(ErrorCode::ResourceAllocationFailed,
                           "no host-visible memory for indirect buffer");
      }
      VkMemoryAllocateInfo indAi{};
      indAi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      indAi.allocationSize = indReq.size;
      indAi.memoryTypeIndex = indMemType;
      r = vkAllocateMemory(device, &indAi, nullptr, &indirectMem);
      if (r != VK_SUCCESS) {
        vkDestroyBuffer(device, indirectBuffer, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vulkanDestroyGraphicsResources(device, resources);
        destroyShaderMods();
        cleanup();
        return vkError("vkAllocateMemory (indirect) failed", r);
      }
      vkBindBufferMemory(device, indirectBuffer, indirectMem, 0);
      void *indMapped = nullptr;
      r = vkMapMemory(device, indirectMem, 0, sizeof(VkDrawIndirectCommand), 0, &indMapped);
      if (r != VK_SUCCESS) {
        vkFreeMemory(device, indirectMem, nullptr);
        vkDestroyBuffer(device, indirectBuffer, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vulkanDestroyGraphicsResources(device, resources);
        destroyShaderMods();
        cleanup();
        return vkError("vkMapMemory (indirect) failed", r);
      }
      std::memcpy(indMapped, &drawCmd, sizeof(drawCmd));
      vkUnmapMemory(device, indirectMem);
      steps.push_back("vkCmdDrawIndirect (smoke)");
    }

    // Offscreen targets: the primed subpass inputs first, then the real colour target at
    // `outputAttachment` -- that one is the only image read back.
    std::vector<VkImage> targetImages(attachmentCount, VK_NULL_HANDLE);
    std::vector<VkDeviceMemory> targetMems(attachmentCount, VK_NULL_HANDLE);
    std::vector<VkImageView> targetViews(attachmentCount, VK_NULL_HANDLE);
    auto destroyTargetImages = [&]() {
      for (VkImageView v : targetViews)
        if (v)
          vkDestroyImageView(device, v, nullptr);
      for (VkDeviceMemory m : targetMems)
        if (m)
          vkFreeMemory(device, m, nullptr);
      for (VkImage i : targetImages)
        if (i)
          vkDestroyImage(device, i, nullptr);
      targetViews.assign(attachmentCount, VK_NULL_HANDLE);
      targetMems.assign(attachmentCount, VK_NULL_HANDLE);
      targetImages.assign(attachmentCount, VK_NULL_HANDLE);
    };

    for (uint32_t i = 0; i < attachmentCount; ++i) {
      VkImageCreateInfo imgCi{};
      imgCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      imgCi.imageType = VK_IMAGE_TYPE_2D;
      imgCi.format = VK_FORMAT_R8G8B8A8_UNORM;
      imgCi.extent = {extentW, extentH, 1};
      imgCi.mipLevels = 1;
      imgCi.arrayLayers = 1;
      imgCi.samples = VK_SAMPLE_COUNT_1_BIT;
      imgCi.tiling = VK_IMAGE_TILING_OPTIMAL;
      imgCi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      imgCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      r = vkCreateImage(device, &imgCi, nullptr, &targetImages[i]);
      if (r != VK_SUCCESS) {
        destroyTargetImages();
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vulkanDestroyGraphicsResources(device, resources);
        destroyShaderMods();
        cleanup();
        return vkError("vkCreateImage failed", r);
      }

      VkMemoryRequirements imgReq{};
      vkGetImageMemoryRequirements(device, targetImages[i], &imgReq);
      const uint32_t imgMemType =
          findMemoryType(physical, imgReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (imgMemType == std::numeric_limits<uint32_t>::max()) {
        destroyTargetImages();
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vulkanDestroyGraphicsResources(device, resources);
        destroyShaderMods();
        cleanup();
        return Error::make(ErrorCode::ResourceAllocationFailed, "no device-local memory for color image");
      }
      VkMemoryAllocateInfo imgAi{};
      imgAi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      imgAi.allocationSize = imgReq.size;
      imgAi.memoryTypeIndex = imgMemType;
      r = vkAllocateMemory(device, &imgAi, nullptr, &targetMems[i]);
      if (r != VK_SUCCESS) {
        destroyTargetImages();
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vulkanDestroyGraphicsResources(device, resources);
        destroyShaderMods();
        cleanup();
        return vkError("vkAllocateMemory (image) failed", r);
      }
      vkBindImageMemory(device, targetImages[i], targetMems[i], 0);
    }
    if (inputCount > 0) {
      steps.push_back("created offscreen color image " + std::to_string(extentW) + "x" +
                      std::to_string(extentH) + " (+" + std::to_string(inputCount) +
                      " subpass input attachment(s))");
    } else {
      steps.push_back("created offscreen color image " + std::to_string(extentW) + "x" +
                      std::to_string(extentH));
    }

    for (uint32_t i = 0; i < attachmentCount; ++i) {
      VkImageViewCreateInfo viewCi{};
      viewCi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      viewCi.image = targetImages[i];
      viewCi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewCi.format = VK_FORMAT_R8G8B8A8_UNORM;
      viewCi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      viewCi.subresourceRange.levelCount = 1;
      viewCi.subresourceRange.layerCount = 1;
      r = vkCreateImageView(device, &viewCi, nullptr, &targetViews[i]);
      if (r != VK_SUCCESS) {
        destroyTargetImages();
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vulkanDestroyGraphicsResources(device, resources);
        destroyShaderMods();
        cleanup();
        return vkError("vkCreateImageView failed", r);
      }
    }

    if (inputCount > 0) {
      // The views exist now, so the deferred subpass-input descriptor writes can land.
      const std::vector<VkImageView> inputViews(targetViews.begin(),
                                                targetViews.begin() + inputCount);
      auto wrote = vulkanWriteInputAttachmentDescriptors(device, resources, inputViews);
      if (!wrote) {
        destroyTargetImages();
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        vulkanDestroyGraphicsResources(device, resources);
        destroyShaderMods();
        cleanup();
        return wrote.error();
      }
      steps.push_back("subpass input descriptors: " +
                      std::to_string(resources.inputAttachments.size()) + " binding(s) -> " +
                      std::to_string(inputCount) + " attachment view(s)");
    }

    VkFramebufferCreateInfo fbCi{};
    fbCi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCi.renderPass = renderPass;
    fbCi.attachmentCount = attachmentCount;
    fbCi.pAttachments = targetViews.data();
    fbCi.width = extentW;
    fbCi.height = extentH;
    fbCi.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    r = vkCreateFramebuffer(device, &fbCi, nullptr, &framebuffer);
    if (r != VK_SUCCESS) {
      destroyTargetImages();
      vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyRenderPass(device, renderPass, nullptr);
      vulkanDestroyGraphicsResources(device, resources);
      destroyShaderMods();
      cleanup();
      return vkError("vkCreateFramebuffer failed", r);
    }
    steps.push_back("vkCreateFramebuffer");

    // Staging buffer for pixel readback. One contiguous region per colour target, so every
    // declared output location gets read back instead of just the first.
    const VkDeviceSize imageBytes = static_cast<VkDeviceSize>(extentW) * extentH * 4;
    const VkDeviceSize stagingBytes = imageBytes * outputCount;
    VkBufferCreateInfo bufCi{};
    bufCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCi.size = stagingBytes;
    bufCi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging = VK_NULL_HANDLE;
    r = vkCreateBuffer(device, &bufCi, nullptr, &staging);
    if (r != VK_SUCCESS) {
      vkDestroyFramebuffer(device, framebuffer, nullptr);
      destroyTargetImages();
      vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyRenderPass(device, renderPass, nullptr);
      vulkanDestroyGraphicsResources(device, resources);
      destroyShaderMods();
      cleanup();
      return vkError("vkCreateBuffer (staging) failed", r);
    }
    VkMemoryRequirements bufReq{};
    vkGetBufferMemoryRequirements(device, staging, &bufReq);
    const uint32_t hostMemType = findMemoryType(
        physical, bufReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (hostMemType == std::numeric_limits<uint32_t>::max()) {
      vkDestroyBuffer(device, staging, nullptr);
      vkDestroyFramebuffer(device, framebuffer, nullptr);
      destroyTargetImages();
      vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyRenderPass(device, renderPass, nullptr);
      vulkanDestroyGraphicsResources(device, resources);
      destroyShaderMods();
      cleanup();
      return Error::make(ErrorCode::ResourceAllocationFailed, "no host-visible memory for staging");
    }
    VkMemoryAllocateInfo bufAi{};
    bufAi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufAi.allocationSize = bufReq.size;
    bufAi.memoryTypeIndex = hostMemType;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    r = vkAllocateMemory(device, &bufAi, nullptr, &stagingMem);
    if (r != VK_SUCCESS) {
      vkDestroyBuffer(device, staging, nullptr);
      vkDestroyFramebuffer(device, framebuffer, nullptr);
      destroyTargetImages();
      vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyRenderPass(device, renderPass, nullptr);
      vulkanDestroyGraphicsResources(device, resources);
      destroyShaderMods();
      cleanup();
      return vkError("vkAllocateMemory (staging) failed", r);
    }
    vkBindBufferMemory(device, staging, stagingMem, 0);

    VkCommandPoolCreateInfo poolCi{};
    poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCi.queueFamilyIndex = queueFamily;
    poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    r = vkCreateCommandPool(device, &poolCi, nullptr, &cmdPool);
    if (r != VK_SUCCESS) {
      vkFreeMemory(device, stagingMem, nullptr);
      vkDestroyBuffer(device, staging, nullptr);
      vkDestroyFramebuffer(device, framebuffer, nullptr);
      destroyTargetImages();
      vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyRenderPass(device, renderPass, nullptr);
      vulkanDestroyGraphicsResources(device, resources);
      destroyShaderMods();
      cleanup();
      return vkError("vkCreateCommandPool failed", r);
    }

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cbai, &cmd);

    const double timestampPeriodNs = props.limits.timestampPeriod;
    const bool timestampsSupported =
        timestampPeriodNs > 0.0f && (qprops[queueFamily].timestampValidBits > 0);
    VkQueryPool queryPool = VK_NULL_HANDLE;
    if (timestampsSupported) {
      VkQueryPoolCreateInfo qpci{};
      qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qpci.queryCount = PipelineStats::kGraphicsTimestampCount;
      vkCreateQueryPool(device, &qpci, nullptr, &queryPool);
      steps.push_back("GPU stage timestamps enabled (" +
                      std::to_string(PipelineStats::kGraphicsTimestampCount) +
                      " markers: TOP/VS/FS/COLOR/BOTTOM)");
    } else {
      steps.push_back("GPU timestamps unavailable — using host wall clock");
    }

    const VkQueryPipelineStatisticFlags statFlags = graphicsPipelineStatFlags();
    const uint32_t statCount = countPipelineStatBits(statFlags);
    VkQueryPool statsPool = VK_NULL_HANDLE;
    if (pipeStatsSupported) {
      VkQueryPoolCreateInfo spci{};
      spci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      spci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
      spci.queryCount = 1;
      spci.pipelineStatistics = statFlags;
      if (vkCreateQueryPool(device, &spci, nullptr, &statsPool) != VK_SUCCESS)
        statsPool = VK_NULL_HANDLE;
      else
        steps.push_back("pipeline statistics queries enabled (" + std::to_string(statCount) +
                        " counters)");
    } else {
      steps.push_back("pipelineStatisticsQuery feature unavailable on device");
    }

    VkQueryPool occlusionPool = VK_NULL_HANDLE;
    {
      VkQueryPoolCreateInfo opci{};
      opci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      opci.queryType = VK_QUERY_TYPE_OCCLUSION;
      opci.queryCount = 1;
      if (vkCreateQueryPool(device, &opci, nullptr, &occlusionPool) != VK_SUCCESS)
        occlusionPool = VK_NULL_HANDLE;
      else
        steps.push_back("occlusion query enabled");
    }

    VulkanPerfQuerySession perf =
        vulkanCreatePerfQuerySession(instance, physical, device, queueFamily, steps,
                                     req.vendorCounters);
    if (!perf.available) {
      perf.note = vulkanPerfQueryHint(meta);
      if (!steps.empty() && steps.back().rfind("vendor HW counters:", 0) == 0)
        steps.back() = "vendor HW counters: " + perf.note;
    }

    const int warmupIters = std::max(0, req.warmup);
    const int timedIters = std::max(1, req.iterations);

    struct TimedSample {
      double gpuMs = 0.0;
      PipelineStats counters;
    };

    auto recordAndSubmit = [&](bool withTimestamps, bool withReadback,
                               bool withCounters) -> Expected<TimedSample> {
      TimedSample sample;
      const bool withPerf = withCounters && perf.available;
      const uint32_t submitPasses = (withPerf && perf.passCount > 1) ? perf.passCount : 1;
      std::chrono::steady_clock::time_point tRecord0;
      std::chrono::steady_clock::time_point tRecord1;

      for (uint32_t pass = 0; pass < submitPasses; ++pass) {
        const bool lastPass = (pass + 1 == submitPasses);
        tRecord0 = std::chrono::steady_clock::now();
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        if (lastPass && withCounters && statsPool)
          vkCmdResetQueryPool(cmd, statsPool, 0, 1);
        if (lastPass && withCounters && occlusionPool)
          vkCmdResetQueryPool(cmd, occlusionPool, 0, 1);
        // Reset perf pool once before the first pass (multi-pass accumulates).
        if (withPerf && pass == 0)
          vulkanPerfCmdReset(cmd, perf);
        if (lastPass && withTimestamps && queryPool) {
          vkCmdResetQueryPool(cmd, queryPool, 0, PipelineStats::kGraphicsTimestampCount);
          vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
        }
        if (lastPass && withCounters && statsPool)
          vkCmdBeginQuery(cmd, statsPool, 0, 0);
        if (lastPass && withCounters && occlusionPool)
          vkCmdBeginQuery(cmd, occlusionPool, 0, 0);
        if (withPerf)
          vulkanPerfCmdBegin(cmd, perf);

        // One clear per attachment: the subpass inputs get the priming ramp, the colour
        // outputs keep the usual opaque black. `clearValueCount` has to cover every
        // attachment the render pass declares, so build the vector unconditionally -- with
        // MRT there is more than one even when there are no subpass inputs.
        std::vector<VkClearValue> clears(attachmentCount);
        for (uint32_t i = 0; i < inputCount; ++i) {
          const float v = inputAttachmentPrimeValue(i, inputCount);
          clears[i].color.float32[0] = v;
          clears[i].color.float32[1] = v;
          clears[i].color.float32[2] = v;
          clears[i].color.float32[3] = 1.0f;
        }
        for (uint32_t i = inputCount; i < attachmentCount; ++i)
          clears[i].color.float32[3] = 1.0f;

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = renderPass;
        rpBegin.framebuffer = framebuffer;
        rpBegin.renderArea.extent = {extentW, extentH};
        rpBegin.clearValueCount = static_cast<uint32_t>(clears.size());
        rpBegin.pClearValues = clears.data();

        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        if (inputCount > 0)
          vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vulkanCmdBindGraphicsResources(cmd, resources);
        if (lastPass && withTimestamps && queryPool)
          vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, queryPool, 1);
        if (useMeshPipeline) {
          cmdDrawMeshTasksEXT(cmd, 1, 1, 1);
        } else if (indirectBuffer) {
          vkCmdDrawIndirect(cmd, indirectBuffer, 0, 1, sizeof(VkDrawIndirectCommand));
        } else {
          vkCmdDraw(cmd, resources.hasVertexAttrs ? resources.vertexCount : 3, 1, 0, 0);
        }
        if (lastPass && withTimestamps && queryPool)
          vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, queryPool, 2);
        vkCmdEndRenderPass(cmd);
        if (lastPass && withTimestamps && queryPool)
          vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, queryPool, 3);

        if (withPerf)
          vulkanPerfCmdEnd(cmd, perf);
        if (lastPass && withCounters && occlusionPool)
          vkCmdEndQuery(cmd, occlusionPool, 0);
        if (lastPass && withCounters && statsPool)
          vkCmdEndQuery(cmd, statsPool, 0);

        if (lastPass && withReadback) {
          for (uint32_t t = 0; t < outputCount; ++t) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = imageBytes * t;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {extentW, extentH, 1};
            vkCmdCopyImageToBuffer(cmd, targetImages[outputAttachment + t],
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
          }
        }
        if (lastPass && withTimestamps && queryPool)
          vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 4);

        vkEndCommandBuffer(cmd);
        tRecord1 = std::chrono::steady_clock::now();
        if (pass == 0)
          sample.counters.hostRecordMs =
              std::chrono::duration<double, std::milli>(tRecord1 - tRecord0).count();

        const auto tSubmit0 = std::chrono::steady_clock::now();
        VkPerformanceQuerySubmitInfoKHR perfSubmit{};
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        if (withPerf && perf.passCount > 1) {
          perfSubmit.sType = VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR;
          perfSubmit.counterPassIndex = pass;
          submit.pNext = &perfSubmit;
        }
        VkResult sr = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        if (sr != VK_SUCCESS)
          return vkError("vkQueueSubmit failed", sr);
        const auto tSubmitEnd = std::chrono::steady_clock::now();
        sample.counters.hostSubmitMs +=
            std::chrono::duration<double, std::milli>(tSubmitEnd - tSubmit0).count();
      }

      const auto tWait0 = std::chrono::steady_clock::now();
      vkQueueWaitIdle(queue);
      const auto tWait1 = std::chrono::steady_clock::now();
      sample.counters.hostQueueWaitMs =
          std::chrono::duration<double, std::milli>(tWait1 - tWait0).count();
      sample.counters.drawCount = 1;

      if (withCounters && statsPool) {
        const double savedSubmit = sample.counters.hostSubmitMs;
        const double savedWait = sample.counters.hostQueueWaitMs;
        const double savedRecord = sample.counters.hostRecordMs;
        std::vector<uint64_t> raw(statCount, 0);
        if (vulkanGetQueryResultsU64(device, statsPool, 0, 1, raw.data(), sizeof(uint64_t)))
          sample.counters = parsePipelineStats(statFlags, raw.data(), statCount);
        sample.counters.hostRecordMs = savedRecord;
        sample.counters.hostSubmitMs = savedSubmit;
        sample.counters.hostQueueWaitMs = savedWait;
        sample.counters.drawCount = 1;
      }
      if (withCounters && occlusionPool) {
        uint64_t occ = 0;
        if (vulkanGetQueryResultsU64(device, occlusionPool, 0, 1, &occ, sizeof(uint64_t))) {
          sample.counters.occlusionAvailable = true;
          sample.counters.occlusionSamplesPassed = occ;
        }
      }
      if (withPerf) {
        std::string vNote;
        if (vulkanPerfQueryResults(device, perf, sample.counters.vendor, vNote)) {
          sample.counters.vendorApi = "VK_KHR_performance_query";
          sample.counters.vendorNote = vNote;
        } else if (!vNote.empty()) {
          sample.counters.vendorApi = "VK_KHR_performance_query";
          sample.counters.vendorNote = vNote;
        }
      }

      if (withTimestamps && queryPool) {
        uint64_t stamps[PipelineStats::kGraphicsTimestampCount] = {};
        if (vulkanGetQueryResultsU64(device, queryPool, 0, PipelineStats::kGraphicsTimestampCount,
                                     stamps, sizeof(uint64_t)) &&
            PipelineStats::fillGraphicsGpuStages(sample.counters.gpuStages, stamps,
                                                 PipelineStats::kGraphicsTimestampCount,
                                                 timestampPeriodNs)) {
          sample.gpuMs = sample.counters.gpuStages.totalMs;
          sample.counters.gpuElapsedMs = sample.gpuMs;
          sample.counters.gpuTimestampAvailable = true;
        } else if (stamps[PipelineStats::kGraphicsTimestampCount - 1] >= stamps[0] &&
                   stamps[PipelineStats::kGraphicsTimestampCount - 1] != 0) {
          sample.gpuMs =
              (static_cast<double>(stamps[PipelineStats::kGraphicsTimestampCount - 1] -
                                   stamps[0]) *
               timestampPeriodNs) /
              1.0e6;
          sample.counters.gpuElapsedMs = sample.gpuMs;
          sample.counters.gpuTimestampAvailable = true;
        } else {
          // Soft-fail: RADV can return VK_NOT_READY (1) around perf-query passes.
          sample.gpuMs = sample.counters.hostSubmitMs + sample.counters.hostQueueWaitMs;
        }
      } else {
        sample.gpuMs = sample.counters.hostSubmitMs + sample.counters.hostQueueWaitMs;
      }
      return sample;
    };

    auto destroyGfx = [&]() {
      vulkanDestroyPerfQuerySession(device, perf);
      if (occlusionPool)
        vkDestroyQueryPool(device, occlusionPool, nullptr);
      if (statsPool)
        vkDestroyQueryPool(device, statsPool, nullptr);
      if (queryPool)
        vkDestroyQueryPool(device, queryPool, nullptr);
      vkDestroyCommandPool(device, cmdPool, nullptr);
      if (indirectMem)
        vkFreeMemory(device, indirectMem, nullptr);
      if (indirectBuffer)
        vkDestroyBuffer(device, indirectBuffer, nullptr);
      vkFreeMemory(device, stagingMem, nullptr);
      vkDestroyBuffer(device, staging, nullptr);
      vkDestroyFramebuffer(device, framebuffer, nullptr);
      destroyTargetImages();
      vkDestroyPipeline(device, pipeline, nullptr);
      vkDestroyRenderPass(device, renderPass, nullptr);
      vulkanDestroyGraphicsResources(device, resources);
      destroyShaderMods();
      cleanup();
    };

    for (int i = 0; i < warmupIters; ++i) {
      auto w = recordAndSubmit(false, false, false);
      if (!w) {
        destroyGfx();
        return w.error();
      }
    }

    out.timingSource = timestampsSupported ? TimingSource::GpuTimestamp : TimingSource::CpuWall;
    std::vector<PipelineStats> counterSamples;
    counterSamples.reserve(static_cast<size_t>(timedIters));
    for (int i = 0; i < timedIters; ++i) {
      const bool last = (i + 1 == timedIters);
      auto sample = recordAndSubmit(timestampsSupported, last, true);
      if (!sample) {
        destroyGfx();
        return sample.error();
      }
      out.samplesMs.push_back(sample.value().gpuMs);
      counterSamples.push_back(sample.value().counters);
    }
    out.counters = PipelineStats::medianOf(counterSamples);
    if (out.counters.vendorNote.empty() && !perf.note.empty())
      out.counters.vendorNote = perf.note;
    out.hostMs = out.samplesMs.empty() ? 0.0 : out.samplesMs.back();
    out.executed = true;
    steps.push_back("vkCmdDraw timed: warmup=" + std::to_string(warmupIters) +
                    " iterations=" + std::to_string(timedIters) + " source=" +
                    std::string(toString(out.timingSource)));
    if (out.counters.available || out.counters.occlusionAvailable)
      steps.push_back(out.counters.summaryLine() + " (median over timed iters)");
    if (!out.counters.vendor.empty() || !out.counters.vendorNote.empty()) {
      const std::string report = out.counters.checklistReport();
      const auto nl = report.find('\n');
      steps.push_back(nl == std::string::npos ? report : report.substr(0, nl));
    }

    void *mapped = nullptr;
    r = vkMapMemory(device, stagingMem, 0, stagingBytes, 0, &mapped);
    if (r == VK_SUCCESS && mapped) {
      const auto *px = static_cast<const std::uint8_t *>(mapped);
      const size_t targetBytes = static_cast<size_t>(imageBytes);
      const size_t frameBytes = static_cast<size_t>(stagingBytes);
      const std::vector<std::uint8_t> frame1(px, px + frameBytes);
      vkUnmapMemory(device, stagingMem);
      mapped = nullptr;

      // Target 0 fills the single-target result fields; every target is checked, so an MRT
      // shader only passes when all of its declared output locations were really written.
      out.colorTargetsChecked = outputCount;
      std::uint8_t center[4] = {0, 0, 0, 0};
      frameCenterPixel(frame1.data(), extentW, extentH, center);
      out.centerR = center[0];
      out.centerG = center[1];
      out.centerB = center[2];
      out.centerA = center[3];
      out.pixelChecked = true;
      steps.push_back("readback center pixel rgba=(" + std::to_string(out.centerR) + "," +
                      std::to_string(out.centerG) + "," + std::to_string(out.centerB) + "," +
                      std::to_string(out.centerA) + ")");

      // Re-submit and read back a second time to confirm the draw reproduces.
      std::vector<std::uint8_t> frame2;
      if (auto again = recordAndSubmit(false, true, false)) {
        void *mapped2 = nullptr;
        if (vkMapMemory(device, stagingMem, 0, stagingBytes, 0, &mapped2) == VK_SUCCESS &&
            mapped2) {
          const auto *p2 = static_cast<const std::uint8_t *>(mapped2);
          frame2.assign(p2, p2 + frameBytes);
          vkUnmapMemory(device, stagingMem);
        }
      }
      if (frame2.empty())
        steps.push_back("warning: second readback unavailable; determinism check skipped");

      // Thin geometry (lines/points/mesh) legitimately covers a tiny fraction of the
      // frame, so the coverage and colour-variety bars are relaxed for those stages.
      // The reported coverage percentage still makes that sparsity visible.
      const bool thinGeometry =
          gMod != VK_NULL_HANDLE || tcMod != VK_NULL_HANDLE || meshMod != VK_NULL_HANDLE;
      FrameCheckConfig cfg;
      if (thinGeometry) {
        cfg.minCoveragePercent = 0.0;
        cfg.minDistinctColors = 1;
      }

      const auto targetName = [&](uint32_t t) -> std::string {
        if (outputCount <= 1)
          return {};
        return " target " + std::to_string(t);
      };

      // Every colour target gets its own determinism diff and frame check; `out.ok` is the
      // conjunction, so a shader whose second target stayed black fails the run.
      bool allOk = true;
      for (uint32_t t = 0; t < outputCount; ++t) {
        const std::uint8_t *t1 = frame1.data() + targetBytes * t;
        const std::uint8_t *t2 =
            frame2.empty() ? nullptr : frame2.data() + targetBytes * t;

        if (t > 0) {
          std::uint8_t c[4] = {0, 0, 0, 0};
          frameCenterPixel(t1, extentW, extentH, c);
          steps.push_back("readback" + targetName(t) + " center pixel rgba=(" +
                          std::to_string(c[0]) + "," + std::to_string(c[1]) + "," +
                          std::to_string(c[2]) + "," + std::to_string(c[3]) + ")");
        }

        if (t2) {
          // compareMask is RGB-only by default: shaders that assign just `.rgb` leave the
          // alpha output component undefined, and drivers fill it with register garbage,
          // so alpha divergence between two identical draws proves nothing. RGB still
          // catches real divergence -- ssao and terraintessellation fail this way.
          const FrameDiff diff = diffFrames(t1, t2, targetBytes, extentW, extentH, cfg);
          if (diff.hasDifference)
            steps.push_back("determinism" + targetName(t) + ": " +
                            std::to_string(diff.differingTexels) + " differing texels (" +
                            std::to_string(static_cast<int>(diff.percentDiffering + 0.5)) +
                            "%), max channel delta " + std::to_string(diff.maxChannelDelta) +
                            ", first at (" + std::to_string(diff.firstX) + "," +
                            std::to_string(diff.firstY) + ")");
        }

        const FrameCheckResult check =
            checkFrame(t1, targetBytes, extentW, extentH, cfg, t2, frame2.empty() ? 0
                                                                                   : targetBytes);
        allOk = allOk && check.ok;
        steps.push_back("frame check" + targetName(t) + ": " + check.reason);
        if (!check.ok) {
          steps.push_back("FAIL: colour target " + std::to_string(t) + " of " +
                          std::to_string(outputCount) +
                          " failed evidence checks (coverage/colours/determinism)");
        }
      }
      out.ok = allOk;
      if (!allOk) {
        // Disambiguate the two ways a frame can be blank using the occlusion query:
        // samples passed with no visible output means the shader ran but wrote nothing
        // (e.g. cleared/discarded colour); no samples means the geometry never covered.
        if (out.counters.occlusionAvailable) {
          if (out.counters.occlusionSamplesPassed > 0 && outputCount == 1)
            steps.push_back("diagnosis: " + std::to_string(out.counters.occlusionSamplesPassed) +
                            " occlusion samples passed, so geometry rasterised but produced no "
                            "visible colour (fragment shader wrote black or discarded)");
          else if (out.counters.occlusionSamplesPassed == 0)
            steps.push_back("diagnosis: 0 occlusion samples passed -- geometry did not cover the "
                            "frame at all");
        }
        steps.push_back("FAIL: rendered frame failed evidence checks (coverage/colours/determinism)");
      }
    } else {
      // Never auto-pass: a failed readback means we have no evidence the draw worked.
      out.ok = false;
      steps.push_back("FAIL: could not map staging memory for pixel check (vkMapMemory rc=" +
                      std::to_string(static_cast<int>(r)) + ")");
    }

    vulkanDestroyPerfQuerySession(device, perf);
    if (occlusionPool)
      vkDestroyQueryPool(device, occlusionPool, nullptr);
    if (statsPool)
      vkDestroyQueryPool(device, statsPool, nullptr);
    if (queryPool)
      vkDestroyQueryPool(device, queryPool, nullptr);
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkFreeMemory(device, stagingMem, nullptr);
    vkDestroyBuffer(device, staging, nullptr);
    vkDestroyFramebuffer(device, framebuffer, nullptr);
    destroyTargetImages();
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyRenderPass(device, renderPass, nullptr);
    vulkanDestroyGraphicsResources(device, resources);
    destroyShaderMods();
    steps.push_back("cleanup graphics objects");
  }

  cleanup();
  steps.push_back("vkDestroyDevice + vkDestroyInstance");
  logInfo("runtime", "Vulkan SPIR-V " + out.mode + (out.ok ? " OK" : " FAIL") + " on " +
                         out.deviceName + (out.executed ? " (executed)" : ""));
  return out;
}

} // namespace csrun
