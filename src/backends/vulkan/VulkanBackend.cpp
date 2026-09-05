#include "csrun/backends/vulkan/VulkanBackend.h"

#include "csrun/benchmark/Statistics.h"
#include "csrun/core/Log.h"
#include "csrun/core/Process.h"
#include "csrun/validation/Comparator.h"

#include <vulkan/vulkan.h>

#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

namespace csrun {
namespace {

constexpr uint32_t kWorkgroupSize = 256;

std::string vkResultString(VkResult r) {
  return "VkResult=" + std::to_string(static_cast<int>(r));
}

Error vkError(ErrorCode code, const std::string &msg, VkResult r) {
  Error e = Error::make(code, msg + " (" + vkResultString(r) + ")");
  e.backend = "vulkan";
  return e;
}

Expected<std::filesystem::path> findGlslang() {
  auto probe = runProcess({"which", "glslangValidator"});
  if (probe && probe.value().exitCode == 0) {
    std::string path = probe.value().stdoutText;
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
      path.pop_back();
    if (!path.empty())
      return std::filesystem::path(path);
  }
  if (std::filesystem::exists("/usr/bin/glslangValidator"))
    return std::filesystem::path("/usr/bin/glslangValidator");
  return Error::make(ErrorCode::ToolchainNotFound,
                     "glslangValidator not found (install glslang-tools)");
}

Expected<std::filesystem::path> findSpirvVal() {
  if (std::filesystem::exists("/usr/bin/spirv-val"))
    return std::filesystem::path("/usr/bin/spirv-val");
  auto probe = runProcess({"which", "spirv-val"});
  if (probe && probe.value().exitCode == 0) {
    std::string path = probe.value().stdoutText;
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
      path.pop_back();
    if (!path.empty())
      return std::filesystem::path(path);
  }
  return Error::make(ErrorCode::ToolchainNotFound, "spirv-val not found (install spirv-tools)");
}

Expected<std::vector<uint32_t>> compileGlslToSpirv(const std::filesystem::path &compPath) {
  auto glslang = findGlslang();
  if (!glslang)
    return glslang.error();
  auto spirvVal = findSpirvVal();
  if (!spirvVal)
    return spirvVal.error();

  const auto outSpv = std::filesystem::temp_directory_path() /
                      ("csrun_vector_add_" + std::to_string(::getpid()) + ".spv");

  auto compile = runProcess({glslang.value().string(), "-V", "-S", "comp",
                             compPath.string(), "-o", outSpv.string()});
  if (!compile)
    return compile.error();
  if (compile.value().exitCode != 0) {
    Error e = Error::make(ErrorCode::CompilationFailed,
                          "glslangValidator failed: " + compile.value().stderrText);
    e.backend = "vulkan";
    e.operation = "compile";
    return e;
  }

  auto validate = runProcess({spirvVal.value().string(), outSpv.string()});
  if (!validate)
    return validate.error();
  if (validate.value().exitCode != 0) {
    Error e = Error::make(ErrorCode::CompilationFailed,
                          "spirv-val failed: " + validate.value().stderrText);
    e.backend = "vulkan";
    e.operation = "spirv-validate";
    return e;
  }

  std::ifstream in(outSpv, std::ios::binary);
  if (!in) {
    return Error::make(ErrorCode::IoError, "failed to read SPIR-V: " + outSpv.string());
  }
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
  std::filesystem::remove(outSpv);
  if (bytes.size() % 4 != 0 || bytes.empty())
    return Error::make(ErrorCode::CompilationFailed, "invalid SPIR-V size");

  std::vector<uint32_t> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  return words;
}

Expected<std::filesystem::path> resolveShaderPath(const Workload &workload) {
  // Prefer sibling .comp next to the YAML source when present.
  if (!workload.sourcePath.empty()) {
    std::filesystem::path yaml(workload.sourcePath);
    auto sibling = yaml.parent_path() / "vector_add.comp";
    if (std::filesystem::exists(sibling))
      return sibling;
  }
  const char *candidates[] = {
      "examples/vector_add/vector_add.comp",
      "../examples/vector_add/vector_add.comp",
      "../../examples/vector_add/vector_add.comp",
  };
  for (const char *c : candidates) {
    if (std::filesystem::exists(c))
      return std::filesystem::path(c);
  }
  return Error::make(ErrorCode::IoError,
                     "vector_add.comp not found; place it next to the workload YAML");
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

struct BufferResource {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
};

Expected<BufferResource> createBuffer(VkDevice device, VkPhysicalDevice phys, VkDeviceSize size,
                                      VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps) {
  BufferResource out;
  out.size = size;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult r = vkCreateBuffer(device, &bi, nullptr, &out.buffer);
  if (r != VK_SUCCESS)
    return vkError(ErrorCode::ResourceAllocationFailed, "vkCreateBuffer failed", r);

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device, out.buffer, &req);
  const uint32_t typeIndex = findMemoryType(phys, req.memoryTypeBits, memProps);
  if (typeIndex == std::numeric_limits<uint32_t>::max()) {
    vkDestroyBuffer(device, out.buffer, nullptr);
    return Error::make(ErrorCode::ResourceAllocationFailed, "no suitable memory type");
  }

  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = typeIndex;
  r = vkAllocateMemory(device, &ai, nullptr, &out.memory);
  if (r != VK_SUCCESS) {
    vkDestroyBuffer(device, out.buffer, nullptr);
    return vkError(ErrorCode::ResourceAllocationFailed, "vkAllocateMemory failed", r);
  }
  vkBindBufferMemory(device, out.buffer, out.memory, 0);
  return out;
}

void destroyBuffer(VkDevice device, BufferResource &b) {
  if (b.buffer)
    vkDestroyBuffer(device, b.buffer, nullptr);
  if (b.memory)
    vkFreeMemory(device, b.memory, nullptr);
  b = {};
}

} // namespace

struct VulkanBackend::Impl {
  // Cached availability snapshot for info().
  bool probed = false;
  bool available = false;
  std::string note = "not probed";
};

VulkanBackend::VulkanBackend() : impl_(std::make_unique<Impl>()) {}
VulkanBackend::~VulkanBackend() = default;

BackendInfo VulkanBackend::info() const {
  BackendInfo i;
  i.type = BackendType::Vulkan;
  i.name = "vulkan";
  auto devices = enumerateDevices();
  if (devices && !devices.value().empty()) {
    i.available = true;
    i.availabilityNote = std::to_string(devices.value().size()) + " device(s)";
  } else {
    i.available = false;
    i.availabilityNote = devices ? "no Vulkan devices found"
                                 : devices.error().message;
  }
  return i;
}

Expected<std::vector<DeviceInfo>> VulkanBackend::enumerateDevices() const {
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "CompilerSutraRun";
  app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  app.pEngineName = "csrun";
  app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
  app.apiVersion = VK_API_VERSION_1_1;

  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;

  VkInstance instance = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&ci, nullptr, &instance);
  if (r != VK_SUCCESS)
    return vkError(ErrorCode::DriverError, "vkCreateInstance failed", r);

  uint32_t count = 0;
  r = vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (r != VK_SUCCESS || count == 0) {
    vkDestroyInstance(instance, nullptr);
    if (r != VK_SUCCESS)
      return vkError(ErrorCode::DriverError, "vkEnumeratePhysicalDevices failed", r);
    return Error::make(ErrorCode::DeviceNotFound, "no Vulkan physical devices");
  }

  std::vector<VkPhysicalDevice> phys(count);
  vkEnumeratePhysicalDevices(instance, &count, phys.data());

  std::vector<DeviceInfo> out;
  for (uint32_t i = 0; i < count; ++i) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys[i], &props);

    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qCount, qprops.data());
    bool hasCompute = false;
    for (const auto &q : qprops) {
      if (q.queueFlags & VK_QUEUE_COMPUTE_BIT) {
        hasCompute = true;
        break;
      }
    }
    if (!hasCompute)
      continue;

    DeviceInfo d;
    d.backend = BackendType::Vulkan;
    d.index = i;
    d.name = props.deviceName;
    d.vendor = "vendorID=0x" + [&] {
      std::ostringstream oss;
      oss << std::hex << props.vendorID;
      return oss.str();
    }();
    d.driver = "api=" + std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." +
               std::to_string(VK_VERSION_MINOR(props.apiVersion));
    d.available = true;
    out.push_back(std::move(d));
  }

  vkDestroyInstance(instance, nullptr);
  if (out.empty())
    return Error::make(ErrorCode::DeviceNotFound, "no Vulkan compute-capable devices");
  return out;
}

Expected<RunResult> VulkanBackend::execute(const Workload &workload,
                                           const DeviceInfo &device) const {
  if (workload.operation != "vector_add") {
    Error e = Error::make(ErrorCode::UnsupportedOperation,
                          "Vulkan Phase 2 supports vector_add only; got " + workload.operation);
    e.backend = "vulkan";
    return e;
  }
  if (workload.inputDtype != DataType::FP32 || workload.outputDtype != DataType::FP32) {
    return Error::make(ErrorCode::UnsupportedDtype, "vector_add Vulkan path requires fp32");
  }
  const uint32_t n = static_cast<uint32_t>(workload.dimensions.count);
  if (n == 0)
    return Error::make(ErrorCode::InvalidWorkload, "vector_add requires dimensions.count > 0");

  auto shaderPath = resolveShaderPath(workload);
  if (!shaderPath)
    return shaderPath.error();
  auto spirv = compileGlslToSpirv(shaderPath.value());
  if (!spirv)
    return spirv.error();

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "CompilerSutraRun";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&ici, nullptr, &instance);
  if (r != VK_SUCCESS)
    return vkError(ErrorCode::DriverError, "vkCreateInstance failed", r);

  uint32_t physCount = 0;
  vkEnumeratePhysicalDevices(instance, &physCount, nullptr);
  std::vector<VkPhysicalDevice> phys(physCount);
  vkEnumeratePhysicalDevices(instance, &physCount, phys.data());
  if (device.index >= physCount) {
    vkDestroyInstance(instance, nullptr);
    return Error::make(ErrorCode::DeviceNotFound, "device index out of range");
  }
  VkPhysicalDevice physical = phys[device.index];

  uint32_t qCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qCount, nullptr);
  std::vector<VkQueueFamilyProperties> qprops(qCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qCount, qprops.data());
  uint32_t computeFamily = std::numeric_limits<uint32_t>::max();
  for (uint32_t i = 0; i < qCount; ++i) {
    if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      computeFamily = i;
      break;
    }
  }
  if (computeFamily == std::numeric_limits<uint32_t>::max()) {
    vkDestroyInstance(instance, nullptr);
    return Error::make(ErrorCode::DeviceNotFound, "no compute queue family");
  }

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = computeFamily;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VkDevice vkDevice = VK_NULL_HANDLE;
  r = vkCreateDevice(physical, &dci, nullptr, &vkDevice);
  if (r != VK_SUCCESS) {
    vkDestroyInstance(instance, nullptr);
    return vkError(ErrorCode::DriverError, "vkCreateDevice failed", r);
  }

  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(vkDevice, computeFamily, 0, &queue);

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physical, &props);
  const double timestampPeriodNs = props.limits.timestampPeriod;

  const VkDeviceSize bytes = static_cast<VkDeviceSize>(n) * sizeof(float);
  auto bufA = createBuffer(vkDevice, physical, bytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto bufB = createBuffer(vkDevice, physical, bytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  auto bufC = createBuffer(vkDevice, physical, bytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!bufA || !bufB || !bufC) {
    if (bufA)
      destroyBuffer(vkDevice, bufA.value());
    if (bufB)
      destroyBuffer(vkDevice, bufB.value());
    if (bufC)
      destroyBuffer(vkDevice, bufC.value());
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(instance, nullptr);
    return bufA ? (bufB ? bufC.error() : bufB.error()) : bufA.error();
  }

  std::vector<float> hostA(n), hostB(n), hostRef(n), hostOut(n);
  for (uint32_t i = 0; i < n; ++i) {
    hostA[i] = static_cast<float>(i) * 0.5f;
    hostB[i] = static_cast<float>(i) * 0.25f + 1.0f;
    hostRef[i] = hostA[i] + hostB[i];
  }

  void *mapped = nullptr;
  vkMapMemory(vkDevice, bufA.value().memory, 0, bytes, 0, &mapped);
  std::memcpy(mapped, hostA.data(), static_cast<size_t>(bytes));
  vkUnmapMemory(vkDevice, bufA.value().memory);
  vkMapMemory(vkDevice, bufB.value().memory, 0, bytes, 0, &mapped);
  std::memcpy(mapped, hostB.data(), static_cast<size_t>(bytes));
  vkUnmapMemory(vkDevice, bufB.value().memory);

  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = spirv.value().size() * sizeof(uint32_t);
  smci.pCode = spirv.value().data();
  VkShaderModule shader = VK_NULL_HANDLE;
  r = vkCreateShaderModule(vkDevice, &smci, nullptr, &shader);
  if (r != VK_SUCCESS) {
    destroyBuffer(vkDevice, bufA.value());
    destroyBuffer(vkDevice, bufB.value());
    destroyBuffer(vkDevice, bufC.value());
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(instance, nullptr);
    return vkError(ErrorCode::CompilationFailed, "vkCreateShaderModule failed", r);
  }

  VkDescriptorSetLayoutBinding bindings[3]{};
  for (uint32_t i = 0; i < 3; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslci.bindingCount = 3;
  dslci.pBindings = bindings;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  vkCreateDescriptorSetLayout(vkDevice, &dslci, nullptr, &setLayout);

  VkPushConstantRange pushRange{};
  pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pushRange.offset = 0;
  pushRange.size = sizeof(uint32_t);

  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &setLayout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pushRange;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  vkCreatePipelineLayout(vkDevice, &plci, nullptr, &pipelineLayout);

  VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = shader;
  cpci.stage.pName = "main";
  cpci.layout = pipelineLayout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  r = vkCreateComputePipelines(vkDevice, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
  if (r != VK_SUCCESS) {
    vkDestroyPipelineLayout(vkDevice, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(vkDevice, setLayout, nullptr);
    vkDestroyShaderModule(vkDevice, shader, nullptr);
    destroyBuffer(vkDevice, bufA.value());
    destroyBuffer(vkDevice, bufB.value());
    destroyBuffer(vkDevice, bufC.value());
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(instance, nullptr);
    return vkError(ErrorCode::CompilationFailed, "vkCreateComputePipelines failed", r);
  }

  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  poolSize.descriptorCount = 3;
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &poolSize;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  vkCreateDescriptorPool(vkDevice, &dpci, nullptr, &pool);

  VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &setLayout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  vkAllocateDescriptorSets(vkDevice, &dsai, &set);

  VkDescriptorBufferInfo bis[3]{};
  BufferResource *bufs[3] = {&bufA.value(), &bufB.value(), &bufC.value()};
  VkWriteDescriptorSet writes[3]{};
  for (uint32_t i = 0; i < 3; ++i) {
    bis[i].buffer = bufs[i]->buffer;
    bis[i].offset = 0;
    bis[i].range = bytes;
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &bis[i];
  }
  vkUpdateDescriptorSets(vkDevice, 3, writes, 0, nullptr);

  VkCommandPoolCreateInfo cpInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpInfo.queueFamilyIndex = computeFamily;
  cpInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool cmdPool = VK_NULL_HANDLE;
  vkCreateCommandPool(vkDevice, &cpInfo, nullptr, &cmdPool);

  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = cmdPool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(vkDevice, &cbai, &cmd);

  const bool timestampsSupported = props.limits.timestampPeriod > 0.0f &&
                                   (qprops[computeFamily].timestampValidBits > 0);
  VkQueryPool queryPool = VK_NULL_HANDLE;
  if (timestampsSupported) {
    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 2;
    vkCreateQueryPool(vkDevice, &qpci, nullptr, &queryPool);
  }

  auto recordAndSubmit = [&](bool withTimestamps) -> Expected<double> {
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    if (withTimestamps && queryPool) {
      vkCmdResetQueryPool(cmd, queryPool, 0, 2);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0,
                            nullptr);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &n);
    const uint32_t groups = (n + kWorkgroupSize - 1) / kWorkgroupSize;
    vkCmdDispatch(cmd, groups, 1, 1);
    if (withTimestamps && queryPool)
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    r = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    if (r != VK_SUCCESS)
      return vkError(ErrorCode::ExecutionFailed, "vkQueueSubmit failed", r);
    vkQueueWaitIdle(queue);

    if (withTimestamps && queryPool) {
      uint64_t stamps[2] = {};
      r = vkGetQueryPoolResults(vkDevice, queryPool, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT);
      if (r != VK_SUCCESS)
        return vkError(ErrorCode::BenchmarkFailed, "vkGetQueryPoolResults failed", r);
      const double ms = (static_cast<double>(stamps[1] - stamps[0]) * timestampPeriodNs) / 1.0e6;
      return ms;
    }
    return 0.0;
  };

  // Warmup
  for (int i = 0; i < workload.execution.warmup; ++i) {
    auto w = recordAndSubmit(false);
    if (!w) {
      // cleanup below via early path — fall through to cleanup helpers
      if (queryPool)
        vkDestroyQueryPool(vkDevice, queryPool, nullptr);
      vkDestroyCommandPool(vkDevice, cmdPool, nullptr);
      vkDestroyDescriptorPool(vkDevice, pool, nullptr);
      vkDestroyPipeline(vkDevice, pipeline, nullptr);
      vkDestroyPipelineLayout(vkDevice, pipelineLayout, nullptr);
      vkDestroyDescriptorSetLayout(vkDevice, setLayout, nullptr);
      vkDestroyShaderModule(vkDevice, shader, nullptr);
      destroyBuffer(vkDevice, bufA.value());
      destroyBuffer(vkDevice, bufB.value());
      destroyBuffer(vkDevice, bufC.value());
      vkDestroyDevice(vkDevice, nullptr);
      vkDestroyInstance(instance, nullptr);
      return w.error();
    }
  }

  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(workload.execution.iterations));
  TimingSource timingSource = TimingSource::CpuWall;
  for (int i = 0; i < workload.execution.iterations; ++i) {
    if (timestampsSupported) {
      auto ms = recordAndSubmit(true);
      if (!ms) {
        // cleanup
        if (queryPool)
          vkDestroyQueryPool(vkDevice, queryPool, nullptr);
        vkDestroyCommandPool(vkDevice, cmdPool, nullptr);
        vkDestroyDescriptorPool(vkDevice, pool, nullptr);
        vkDestroyPipeline(vkDevice, pipeline, nullptr);
        vkDestroyPipelineLayout(vkDevice, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(vkDevice, setLayout, nullptr);
        vkDestroyShaderModule(vkDevice, shader, nullptr);
        destroyBuffer(vkDevice, bufA.value());
        destroyBuffer(vkDevice, bufB.value());
        destroyBuffer(vkDevice, bufC.value());
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(instance, nullptr);
        return ms.error();
      }
      samples.push_back(ms.value());
      timingSource = TimingSource::GpuTimestamp;
    } else {
      const auto t0 = std::chrono::steady_clock::now();
      auto ms = recordAndSubmit(false);
      const auto t1 = std::chrono::steady_clock::now();
      if (!ms) {
        if (queryPool)
          vkDestroyQueryPool(vkDevice, queryPool, nullptr);
        vkDestroyCommandPool(vkDevice, cmdPool, nullptr);
        vkDestroyDescriptorPool(vkDevice, pool, nullptr);
        vkDestroyPipeline(vkDevice, pipeline, nullptr);
        vkDestroyPipelineLayout(vkDevice, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(vkDevice, setLayout, nullptr);
        vkDestroyShaderModule(vkDevice, shader, nullptr);
        destroyBuffer(vkDevice, bufA.value());
        destroyBuffer(vkDevice, bufB.value());
        destroyBuffer(vkDevice, bufC.value());
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(instance, nullptr);
        return ms.error();
      }
      samples.push_back(
          std::chrono::duration<double, std::milli>(t1 - t0).count());
      timingSource = TimingSource::CpuWall;
    }
  }

  vkMapMemory(vkDevice, bufC.value().memory, 0, bytes, 0, &mapped);
  std::memcpy(hostOut.data(), mapped, static_cast<size_t>(bytes));
  vkUnmapMemory(vkDevice, bufC.value().memory);

  ComparePolicy policy;
  policy.absoluteTolerance = workload.validation.absoluteTolerance;
  policy.relativeTolerance = workload.validation.relativeTolerance;
  CompareResult cmp = Comparator::compareFloat32(hostRef, hostOut, policy);

  RunResult result;
  result.schemaVersion = 1;
  result.workloadName = workload.name;
  result.operation = workload.operation;
  result.backend = BackendType::Vulkan;
  result.deviceName = device.name;
  result.deviceVendor = device.vendor;
  result.driver = device.driver;
  result.compilerName = "glslangValidator";
  result.warmup = workload.execution.warmup;
  result.iterations = workload.execution.iterations;
  result.timing = Statistics::compute(samples, timingSource);
  if (!workload.validation.enabled)
    result.validation = ValidationStatus::Skipped;
  else
    result.validation = cmp.passed ? ValidationStatus::Passed : ValidationStatus::Failed;
  result.note = cmp.message;

  if (queryPool)
    vkDestroyQueryPool(vkDevice, queryPool, nullptr);
  vkDestroyCommandPool(vkDevice, cmdPool, nullptr);
  vkDestroyDescriptorPool(vkDevice, pool, nullptr);
  vkDestroyPipeline(vkDevice, pipeline, nullptr);
  vkDestroyPipelineLayout(vkDevice, pipelineLayout, nullptr);
  vkDestroyDescriptorSetLayout(vkDevice, setLayout, nullptr);
  vkDestroyShaderModule(vkDevice, shader, nullptr);
  destroyBuffer(vkDevice, bufA.value());
  destroyBuffer(vkDevice, bufB.value());
  destroyBuffer(vkDevice, bufC.value());
  vkDestroyDevice(vkDevice, nullptr);
  vkDestroyInstance(instance, nullptr);

  if (workload.validation.enabled && !cmp.passed) {
    Error e = Error::make(ErrorCode::ValidationFailed, cmp.message);
    e.backend = "vulkan";
    // Still return structured result via note; surface as error for CLI non-zero.
    logError("validation", cmp.message);
    // Prefer returning the RunResult with failed status so JSON is useful.
  }

  logInfo("runtime", "Vulkan vector_add completed on " + result.deviceName);
  return result;
}

std::unique_ptr<Backend> createVulkanBackend() {
  return std::make_unique<VulkanBackend>();
}

} // namespace csrun
