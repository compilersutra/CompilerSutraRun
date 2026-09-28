#include "csrun/backends/vulkan/VulkanBackend.h"

#include "csrun/backends/vulkan/VulkanDevicePick.h"
#include "csrun/backends/vulkan/VulkanHandle.h"
#include "csrun/backends/vulkan/VulkanPerfQuery.h"
#include "csrun/backends/vulkan/VulkanPipelineQueries.h"
#include "csrun/backends/vulkan/VulkanRayTracingSmoke.h"
#include "csrun/backends/vulkan/VulkanSpirvRunner.h"
#include "csrun/benchmark/Statistics.h"
#include "csrun/benchmark/PipelineStats.h"
#include "csrun/compiler/SpirvToolchain.h"
#include "csrun/core/Log.h"
#include "csrun/core/Process.h"
#include "csrun/workload/VectorAddScaffold.h"

#include <vulkan/vulkan.h>

#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
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

Expected<std::vector<uint32_t>> compileGlslToSpirv(const std::filesystem::path &shaderPath) {
  SpirvCompileRequest req;
  req.input = shaderPath;
  req.output = std::filesystem::temp_directory_path() /
               ("csrun_" + shaderPath.filename().string() + "_" + std::to_string(::getpid()) +
                ".spv");
  req.validate = true;
  req.keepOutput = false;
  auto compiled = SpirvToolchain::compile(req);
  if (!compiled)
    return compiled.error();
  return compiled.value().words;
}

std::filesystem::path resolveShaderRefPath(const Workload &workload, const ShaderRef &ref) {
  namespace fs = std::filesystem;
  if (ref.path.is_absolute())
    return ref.path;
  if (!workload.sourcePath.empty()) {
    const fs::path base = fs::path(workload.sourcePath).parent_path() / ref.path;
    if (fs::exists(base))
      return base;
  }
  if (fs::exists(ref.path))
    return ref.path;
  return ref.path;
}

Expected<RunResult> executeHandwrittenSpirv(const Workload &workload, const DeviceInfo &device,
                                            const ExecuteOptions &options) {
  if (workload.shaders.empty()) {
    return Error::make(ErrorCode::InvalidWorkload,
                       "spirv workload needs shaders: [.vert/.frag/.comp, ...]");
  }

  namespace fs = std::filesystem;
  fs::path outDir = workload.spvOutputDir;
  if (outDir.empty()) {
    if (!workload.sourcePath.empty())
      outDir = fs::path(workload.sourcePath).parent_path() / "spv";
    else
      outDir = "spv";
  }

  std::ostringstream note;
  note << "handwritten SPIR-V pipeline\n";
  VulkanSmokeRequest smoke;
  smoke.deviceIndex = options.deviceIndex
                          ? options.deviceIndex
                          : std::optional<std::uint32_t>{device.index};
  smoke.show = options.showWindowSet ? options.showWindow : workload.execution.show;
  smoke.showMs = workload.execution.showMs;
  smoke.warmup = options.benchmark ? workload.execution.warmup : 0;
  smoke.iterations = options.benchmark ? std::max(1, workload.execution.iterations) : 1;
  smoke.vendorCounters = options.vendorCounters;
  smoke.width = std::max<std::uint32_t>(1, workload.execution.width);
  smoke.height = std::max<std::uint32_t>(1, workload.execution.height);
  smoke.windowWidth = static_cast<int>(smoke.width >= 64 ? smoke.width : 800);
  smoke.windowHeight = static_cast<int>(smoke.height >= 64 ? smoke.height : 600);
  smoke.dimCount = workload.dimensions.count;
  smoke.dimM = workload.dimensions.m;
  smoke.dimN = workload.dimensions.n;
  smoke.dimK = workload.dimensions.k;
  smoke.validate = workload.validation.enabled;
  smoke.absTol = workload.validation.absoluteTolerance;

  for (const auto &ref : workload.shaders) {
    const fs::path src = resolveShaderRefPath(workload, ref);
    ShaderStage stage = ref.stage.empty() ? shaderStageFromPath(src) : parseShaderStage(ref.stage);
    if (stage == ShaderStage::Unknown)
      stage = shaderStageFromPath(src);
    if (stage == ShaderStage::Unknown) {
      Error e = Error::make(ErrorCode::UnsupportedOperation,
                            "cannot detect shader stage for " + src.string());
      e.backend = "vulkan";
      return e;
    }

    SpirvCompileRequest req;
    req.input = src;
    req.output = outDir / (src.filename().string() + ".spv");
    req.validate = true;
    req.keepOutput = true;
    req.stageOverride = stage;
    auto compiled = SpirvToolchain::compile(req);
    if (!compiled)
      return compiled.error();

    auto info = SpirvToolchain::inspectSpvFile(compiled.value().spvPath);
    if (!info)
      return info.error();
    note << "  - " << src.filename().string() << " → " << compiled.value().spvPath.filename().string()
         << " [" << toString(compiled.value().stage) << "] " << info.value().summary << '\n';

    auto dis = SpirvToolchain::disassembleSpvFile(compiled.value().spvPath, 12);
    if (dis && !dis.value().empty())
      note << "    spirv-dis (head):\n" << dis.value();

    if (compiled.value().stage == ShaderStage::Compute)
      smoke.computeSpv = compiled.value().spvPath;
    else if (compiled.value().stage == ShaderStage::Vertex)
      smoke.vertexSpv = compiled.value().spvPath;
    else if (compiled.value().stage == ShaderStage::Fragment)
      smoke.fragmentSpv = compiled.value().spvPath;
    else if (compiled.value().stage == ShaderStage::Geometry)
      smoke.geometrySpv = compiled.value().spvPath;
    else if (compiled.value().stage == ShaderStage::TessControl)
      smoke.tessControlSpv = compiled.value().spvPath;
    else if (compiled.value().stage == ShaderStage::TessEvaluation)
      smoke.tessEvalSpv = compiled.value().spvPath;
    else if (compiled.value().stage == ShaderStage::Mesh)
      smoke.meshSpv = compiled.value().spvPath;
    else if (compiled.value().stage == ShaderStage::Task)
      smoke.taskSpv = compiled.value().spvPath;
    else if (compiled.value().stage == ShaderStage::RayGen)
      smoke.raygenSpv = compiled.value().spvPath;
    else if (compiled.value().stage == ShaderStage::ClosestHit)
      smoke.closestHitSpv = compiled.value().spvPath;
    else if (compiled.value().stage == ShaderStage::Miss)
      smoke.missSpv = compiled.value().spvPath;
    else {
      Error e = Error::make(ErrorCode::UnsupportedOperation,
                            std::string("Vulkan execute does not support shader stage '") +
                                std::string(toString(compiled.value().stage)) + "' yet (" +
                                src.filename().string() + ")");
      e.backend = "vulkan";
      e.operation = "execute";
      return e;
    }
  }

  // Ray-tracing workloads use a dedicated smoke path (AS + SBT + traceRays).
  if (!smoke.raygenSpv.empty()) {
    if (smoke.closestHitSpv.empty() || smoke.missSpv.empty()) {
      Error e = Error::make(ErrorCode::InvalidWorkload,
                            "ray tracing execute needs raygen + closesthit + miss SPIR-V");
      e.backend = "vulkan";
      e.operation = "execute";
      return e;
    }
    VulkanRayTracingRequest rt;
    rt.raygenSpv = smoke.raygenSpv;
    rt.closestHitSpv = smoke.closestHitSpv;
    rt.missSpv = smoke.missSpv;
    rt.deviceIndex = smoke.deviceIndex;
    rt.width = smoke.width;
    rt.height = smoke.height;
    rt.warmup = smoke.warmup;
    rt.iterations = smoke.iterations;
    auto traced = vulkanRayTracingSmoke(rt);
    if (!traced)
      return traced.error();

    for (const auto &step : traced.value().steps)
      note << "  * " << step << '\n';

    RunResult result;
    result.workloadName = workload.name;
    result.operation = workload.operation;
    result.backend = BackendType::Vulkan;
    result.deviceName = traced.value().deviceName.empty() ? device.name : traced.value().deviceName;
    result.deviceVendor = device.vendor;
    result.driver = device.driver;
    result.compilerName = "glslangValidator+spirv-val";
    result.warmup = rt.warmup;
    result.iterations = rt.iterations;
    if (!traced.value().samplesMs.empty())
      result.timing =
          Statistics::compute(traced.value().samplesMs, traced.value().timingSource);
    else
      result.timing.source = TimingSource::CpuWall;
    result.validation =
        traced.value().ok ? ValidationStatus::Passed : ValidationStatus::Failed;
    result.note = note.str() + "mode=" + traced.value().mode +
                  (traced.value().ok ? " PASS (vkCmdTraceRaysKHR + pixel readback)\n"
                                     : " FAIL (ray tracing output is black)\n");
    return result;
  }

  auto loaded = vulkanSmokeSpirv(smoke);
  if (!loaded)
    return loaded.error();

  for (const auto &step : loaded.value().steps)
    note << "  * " << step << '\n';

  RunResult result;
  result.workloadName = workload.name;
  result.operation = workload.operation;
  result.backend = BackendType::Vulkan;
  result.deviceName = loaded.value().deviceName.empty() ? device.name : loaded.value().deviceName;
  result.deviceVendor = device.vendor;
  result.driver = device.driver;
  result.compilerName = "glslangValidator+spirv-val";
  result.warmup = smoke.warmup;
  result.iterations = smoke.iterations;
  if (!loaded.value().samplesMs.empty()) {
    result.timing = Statistics::compute(loaded.value().samplesMs, loaded.value().timingSource);
  } else {
    result.timing.source = TimingSource::CpuWall;
    result.timing.minMs = loaded.value().hostMs;
    result.timing.medianMs = loaded.value().hostMs;
    result.timing.meanMs = loaded.value().hostMs;
    result.timing.maxMs = loaded.value().hostMs;
  }
  result.counters = loaded.value().counters;
  result.note = note.str();
  if (!loaded.value().ok) {
    result.validation = ValidationStatus::Failed;
    result.note += "vulkan execute failed\n";
  } else if (loaded.value().framesPresented > 0) {
    result.validation = ValidationStatus::Passed;
    result.note += "mode=" + loaded.value().mode + " PASS (vkQueuePresentKHR x" +
                   std::to_string(loaded.value().framesPresented) + ")\n";
  } else if (loaded.value().pixelChecked) {
    result.validation = ValidationStatus::Passed;
    // Say how many targets were actually read back: an MRT shader that only checks its
    // first target would otherwise be indistinguishable from a single-target one.
    result.note += "mode=" + loaded.value().mode + " PASS (vkCmdDraw + pixel readback";
    result.note += loaded.value().colorTargetsChecked > 1
                       ? " x" + std::to_string(loaded.value().colorTargetsChecked)
                       : "";
    result.note += ")\n";
  } else if (loaded.value().computeChecked) {
    result.validation = ValidationStatus::Passed;
    result.note += "mode=" + loaded.value().mode + " PASS (compute + host check)\n";
  } else if (loaded.value().executed) {
    result.validation = ValidationStatus::Skipped;
    result.note += "mode=" + loaded.value().mode + " PASS (submitted to GPU)\n";
  } else {
    result.validation = ValidationStatus::Skipped;
    result.note += "mode=" + loaded.value().mode + " PASS\n";
  }

  logInfo("runtime", "Vulkan handwritten SPIR-V workload completed on " + result.deviceName);
  return result;
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

/// A buffer and the device memory bound to it.
///
/// Members are declared so that reverse declaration order destroys the buffer
/// before its memory, which is what Vulkan requires. The owning deleters capture
/// the `VkDevice`, so no manual `destroyBuffer` call is needed on any exit path.
struct BufferResource {
  VkDeviceMemoryHandle memory;
  VkBufferHandle buffer;
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
  VkBuffer rawBuffer = VK_NULL_HANDLE;
  VkResult r = vkCreateBuffer(device, &bi, nullptr, &rawBuffer);
  if (r != VK_SUCCESS)
    return vkError(ErrorCode::ResourceAllocationFailed, "vkCreateBuffer failed", r);
  out.buffer = VkBufferHandle{rawBuffer, {device, vkDestroyBuffer}};

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device, rawBuffer, &req);
  const uint32_t typeIndex = findMemoryType(phys, req.memoryTypeBits, memProps);
  if (typeIndex == std::numeric_limits<uint32_t>::max())
    return Error::make(ErrorCode::ResourceAllocationFailed, "no suitable memory type");

  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = typeIndex;
  VkDeviceMemory rawMemory = VK_NULL_HANDLE;
  r = vkAllocateMemory(device, &ai, nullptr, &rawMemory);
  if (r != VK_SUCCESS)
    return vkError(ErrorCode::ResourceAllocationFailed, "vkAllocateMemory failed", r);
  out.memory = VkDeviceMemoryHandle{rawMemory, {device, vkFreeMemory}};

  vkBindBufferMemory(device, rawBuffer, rawMemory, 0);
  return out;
}

} // namespace

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
  app.apiVersion = VK_API_VERSION_1_2;

  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;

  VkInstance instance = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&ci, nullptr, &instance);
  if (r != VK_SUCCESS)
    return vkError(ErrorCode::DriverError, "vkCreateInstance failed", r);
  VkInstanceHandle instanceGuard{instance};

  uint32_t count = 0;
  r = vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (r != VK_SUCCESS || count == 0) {
    if (r != VK_SUCCESS)
      return vkError(ErrorCode::DriverError, "vkEnumeratePhysicalDevices failed", r);
    return Error::make(ErrorCode::DeviceNotFound, "no Vulkan physical devices");
  }

  std::vector<VkPhysicalDevice> phys(count);
  vkEnumeratePhysicalDevices(instance, &count, phys.data());

  struct Scored {
    DeviceInfo info;
    int score = 0;
  };
  std::vector<Scored> scored;
  for (uint32_t i = 0; i < count; ++i) {
    VulkanDeviceMeta meta = vulkanQueryDeviceMeta(phys[i], i);
    if (!meta.hasCompute)
      continue;
    Scored s;
    s.score = vulkanDeviceScore(meta);
    s.info.backend = BackendType::Vulkan;
    s.info.index = i;
    s.info.name = meta.name + " [" + meta.typeName + "] " + meta.driverName +
                  (meta.hasPerfQuery ? " perf_query=yes" : " perf_query=no");
    s.info.vendor = "vendorID via " + meta.driverName;
    s.info.driver = meta.driverInfo.empty() ? meta.driverName : meta.driverInfo;
    s.info.available = true;
    scored.push_back(std::move(s));
  }

  if (scored.empty())
    return Error::make(ErrorCode::DeviceNotFound, "no Vulkan compute-capable devices");

  const bool hasRealGpu = std::any_of(scored.begin(), scored.end(), [](const Scored &e) {
    return e.score >= 1000;
  });
  if (hasRealGpu) {
    std::vector<Scored> filtered;
    for (auto &e : scored) {
      if (e.score >= 1000)
        filtered.push_back(std::move(e));
    }
    if (!filtered.empty())
      scored = std::move(filtered);
  }

  std::stable_sort(scored.begin(), scored.end(),
                   [](const Scored &a, const Scored &b) { return a.score > b.score; });

  std::vector<DeviceInfo> out;
  out.reserve(scored.size());
  for (auto &e : scored)
    out.push_back(std::move(e.info));
  return out;
}

Expected<RunResult> VulkanBackend::execute(const Workload &workload,
                                           const DeviceInfo &device,
                                           const ExecuteOptions &options) const {
  if (isSpirvWorkload(workload))
    return executeHandwrittenSpirv(workload, device, options);

  auto nOr = requireVectorAddFp32(workload, "vulkan");
  if (!nOr)
    return nOr.error();
  const uint32_t n = nOr.value();

  static constexpr std::string_view kFallbacks[] = {
      "examples/vector_add/vector_add.comp",
      "../examples/vector_add/vector_add.comp",
      "../../examples/vector_add/vector_add.comp",
  };
  auto shaderPath = resolveKernelPath(workload, "vector_add.comp", kFallbacks);
  if (!shaderPath)
    return shaderPath.error();
  auto spirv = compileGlslToSpirv(shaderPath.value());
  if (!spirv)
    return spirv.error();

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "CompilerSutraRun";
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&ici, nullptr, &instance);
  if (r != VK_SUCCESS)
    return vkError(ErrorCode::DriverError, "vkCreateInstance failed", r);
  // Owns `instance` for the rest of the function. Declared first among the guards below
  // so it is destroyed last; every early return after this point is leak-free.
  VkInstanceHandle instanceGuard{instance};

  uint32_t physCount = 0;
  vkEnumeratePhysicalDevices(instance, &physCount, nullptr);
  std::vector<VkPhysicalDevice> phys(physCount);
  vkEnumeratePhysicalDevices(instance, &physCount, phys.data());
  if (physCount == 0)
    return Error::make(ErrorCode::DeviceNotFound, "no Vulkan physical devices");
  const uint32_t physIndex = vulkanPickPhysicalDevice(
      phys, options.deviceIndex ? options.deviceIndex : std::optional<std::uint32_t>{device.index},
      false);
  if (physIndex >= physCount)
    return Error::make(ErrorCode::DeviceNotFound, "device index out of range");
  VkPhysicalDevice physical = phys[physIndex];
  const VulkanDeviceMeta meta = vulkanQueryDeviceMeta(physical, physIndex);

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
  if (computeFamily == std::numeric_limits<uint32_t>::max())
    return Error::make(ErrorCode::DeviceNotFound, "no compute queue family");

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = computeFamily;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;

  VkPhysicalDeviceFeatures supportedFeatures{};
  vkGetPhysicalDeviceFeatures(physical, &supportedFeatures);
  const bool pipeStatsSupported = supportedFeatures.pipelineStatisticsQuery == VK_TRUE;
  const VulkanPerfDeviceSetup perfSetup = vulkanPreparePerfQueryDevice(physical);
  std::vector<const char *> deviceExts;
  if (perfSetup.want)
    deviceExts.push_back(VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME);

  VkPhysicalDeviceFeatures enabledFeatures{};
  if (pipeStatsSupported)
    enabledFeatures.pipelineStatisticsQuery = VK_TRUE;

  VkPhysicalDevicePerformanceQueryFeaturesKHR enabledPerfFeatures = perfSetup.features;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.features = enabledFeatures;

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
  dci.ppEnabledExtensionNames = deviceExts.empty() ? nullptr : deviceExts.data();
  if (perfSetup.want) {
    enabledPerfFeatures.pNext = nullptr;
    features2.pNext = &enabledPerfFeatures;
    dci.pNext = &features2;
    dci.pEnabledFeatures = nullptr;
  } else {
    dci.pEnabledFeatures = &enabledFeatures;
  }
  VkDevice vkDevice = VK_NULL_HANDLE;
  r = vkCreateDevice(physical, &dci, nullptr, &vkDevice);
  if (r != VK_SUCCESS)
    return vkError(ErrorCode::DriverError, "vkCreateDevice failed", r);
  // Declared after instanceGuard so it is destroyed before the instance.
  VkDeviceHandle deviceGuard{vkDevice};

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
  if (!bufA || !bufB || !bufC)
    return bufA ? (bufB ? bufC.error() : bufB.error()) : bufA.error();

  VectorAddHost host = makeVectorAddHost(n);
  const int warmupIters = options.benchmark ? workload.execution.warmup : 0;
  const int timedIters = options.benchmark ? workload.execution.iterations : 1;

  double h2dMs = 0.0;
  {
    const auto t0 = std::chrono::steady_clock::now();
    void *mapped = nullptr;
    vkMapMemory(vkDevice, bufA.value().memory.get(), 0, bytes, 0, &mapped);
    std::memcpy(mapped, host.a.data(), static_cast<size_t>(bytes));
    vkUnmapMemory(vkDevice, bufA.value().memory.get());
    vkMapMemory(vkDevice, bufB.value().memory.get(), 0, bytes, 0, &mapped);
    std::memcpy(mapped, host.b.data(), static_cast<size_t>(bytes));
    vkUnmapMemory(vkDevice, bufB.value().memory.get());
    const auto t1 = std::chrono::steady_clock::now();
    h2dMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  }

  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = spirv.value().size() * sizeof(uint32_t);
  smci.pCode = spirv.value().data();
  VkShaderModule shader = VK_NULL_HANDLE;
  r = vkCreateShaderModule(vkDevice, &smci, nullptr, &shader);
  if (r != VK_SUCCESS)
    return vkError(ErrorCode::CompilationFailed, "vkCreateShaderModule failed", r);
  VkShaderModuleHandle shaderGuard{shader, {vkDevice, vkDestroyShaderModule}};

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
  VkDescriptorSetLayoutHandle setLayoutGuard{setLayout, {vkDevice, vkDestroyDescriptorSetLayout}};

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
  VkPipelineLayoutHandle pipelineLayoutGuard{pipelineLayout,
                                            {vkDevice, vkDestroyPipelineLayout}};

  VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = shader;
  cpci.stage.pName = "main";
  cpci.layout = pipelineLayout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  r = vkCreateComputePipelines(vkDevice, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
  if (r != VK_SUCCESS)
    return vkError(ErrorCode::CompilationFailed, "vkCreateComputePipelines failed", r);
  VkPipelineHandle pipelineGuard{pipeline, {vkDevice, vkDestroyPipeline}};

  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  poolSize.descriptorCount = 3;
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &poolSize;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  vkCreateDescriptorPool(vkDevice, &dpci, nullptr, &pool);
  VkDescriptorPoolHandle poolGuard{pool, {vkDevice, vkDestroyDescriptorPool}};

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
    bis[i].buffer = bufs[i]->buffer.get();
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
  VkCommandPoolHandle cmdPoolGuard{cmdPool, {vkDevice, vkDestroyCommandPool}};

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
    qpci.queryCount = PipelineStats::kComputeTimestampCount;
    vkCreateQueryPool(vkDevice, &qpci, nullptr, &queryPool);
  }
  VkQueryPoolHandle queryPoolGuard{queryPool, {vkDevice, vkDestroyQueryPool}};

  PipelineStats pipeCounters;
  std::vector<PipelineStats> counterSamples;
  VkQueryPool statsPool = VK_NULL_HANDLE;
  if (pipeStatsSupported) {
    VkQueryPoolCreateInfo spci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    spci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    spci.queryCount = 1;
    spci.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
    if (vkCreateQueryPool(vkDevice, &spci, nullptr, &statsPool) != VK_SUCCESS)
      statsPool = VK_NULL_HANDLE;
  }
  VkQueryPoolHandle statsPoolGuard{statsPool, {vkDevice, vkDestroyQueryPool}};

  std::vector<std::string> perfSteps;
  VulkanPerfQuerySession perf =
      vulkanCreatePerfQuerySession(instance, physical, vkDevice, computeFamily, perfSteps,
                                   options.vendorCounters);
  if (!perf.available)
    perf.note = vulkanPerfQueryHint(meta);

  // Released on scope exit; the ordering below guarantees the perf session and both query
  // pools are torn down before the device, so no exit path needs an explicit cleanup call.
  struct PerfSessionGuard {
    VkDevice device;
    VulkanPerfQuerySession session;
    ~PerfSessionGuard() { vulkanDestroyPerfQuerySession(device, session); }
  } perfGuard{vkDevice, perf};

  auto recordAndSubmit = [&](bool withTimestamps, bool withStats) -> Expected<double> {
    PipelineStats iterCounters;
    const bool withPerf = withStats && perf.available;
    const uint32_t submitPasses = (withPerf && perf.passCount > 1) ? perf.passCount : 1;
    double gpuMs = 0.0;

    for (uint32_t pass = 0; pass < submitPasses; ++pass) {
      const bool lastPass = (pass + 1 == submitPasses);
      const auto tRecord0 = std::chrono::steady_clock::now();
      vkResetCommandBuffer(cmd, 0);
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
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
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0,
                              nullptr);
      vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &n);
      const uint32_t groups = (n + kWorkgroupSize - 1) / kWorkgroupSize;
      vkCmdDispatch(cmd, groups, 1, 1);
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
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &cmd;
      if (withPerf && perf.passCount > 1) {
        perfSubmit.sType = VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR;
        perfSubmit.counterPassIndex = pass;
        submit.pNext = &perfSubmit;
      }
      r = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
      if (r != VK_SUCCESS)
        return vkError(ErrorCode::ExecutionFailed, "vkQueueSubmit failed", r);
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
      if (vulkanGetQueryResultsU64(vkDevice, statsPool, 0, 1, &csInv, sizeof(uint64_t))) {
        iterCounters.available = true;
        iterCounters.csInvocations = csInv;
      }
    }
    if (withPerf) {
      std::string vNote;
      if (vulkanPerfQueryResults(vkDevice, perf, iterCounters.vendor, vNote)) {
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
      if (vulkanGetQueryResultsU64(vkDevice, queryPool, 0, PipelineStats::kComputeTimestampCount,
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
      return iterCounters.hostSubmitMs + iterCounters.hostQueueWaitMs;
    }
    return iterCounters.hostSubmitMs;
  };

  // Warmup
  for (int i = 0; i < warmupIters; ++i) {
    auto w = recordAndSubmit(false, false);
    if (!w)
      return w.error();
  }

  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(timedIters));
  TimingSource timingSource = TimingSource::CpuWall;
  for (int i = 0; i < timedIters; ++i) {
    const bool last = (i + 1 == timedIters);
    if (timestampsSupported) {
      auto ms = recordAndSubmit(true, true);
      if (!ms)
        return ms.error();
      samples.push_back(ms.value());
      timingSource = TimingSource::GpuTimestamp;
    } else {
      auto ms = recordAndSubmit(false, true);
      if (!ms)
        return ms.error();
      samples.push_back(ms.value());
      timingSource = TimingSource::CpuWall;
    }
    (void)last;
  }

  pipeCounters = PipelineStats::medianOf(counterSamples);
  if (pipeCounters.vendorNote.empty() && !perf.note.empty())
    pipeCounters.vendorNote = perf.note;
  pipeCounters.hostH2dMs = h2dMs;

  {
    const auto t0 = std::chrono::steady_clock::now();
    void *mapped = nullptr;
    vkMapMemory(vkDevice, bufC.value().memory.get(), 0, bytes, 0, &mapped);
    std::memcpy(host.output.data(), mapped, static_cast<size_t>(bytes));
    vkUnmapMemory(vkDevice, bufC.value().memory.get());
    const auto t1 = std::chrono::steady_clock::now();
    pipeCounters.hostD2hMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  }

  RunResult result = fillVectorAddResult(workload, device, BackendType::Vulkan, "glslangValidator",
                                         options, samples, timingSource, host.reference, host.output);
  result.counters = pipeCounters;
  if (pipeCounters.available)
    result.note = (result.note.empty() ? "" : result.note + "\n") + pipeCounters.summaryLine();
  if (!pipeCounters.vendor.empty() || !pipeCounters.vendorNote.empty()) {
    const std::string report = pipeCounters.checklistReport();
    const auto nl = report.find('\n');
    result.note = (result.note.empty() ? "" : result.note + "\n") +
                  (nl == std::string::npos ? report : report.substr(0, nl));
  }

  if (result.validation == ValidationStatus::Failed)
    logError("validation", result.note);

  logInfo("runtime", "Vulkan vector_add completed on " + result.deviceName);
  return result;
}

std::unique_ptr<Backend> createVulkanBackend() {
  return std::make_unique<VulkanBackend>();
}

} // namespace csrun
