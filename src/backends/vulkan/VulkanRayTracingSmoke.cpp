//===----------------------------------------------------------------------===//
//
// VulkanRayTracingSmoke.cpp — minimal triangle BLAS/TLAS ray tracing smoke
//
//===----------------------------------------------------------------------===//

#include "csrun/backends/vulkan/VulkanRayTracingSmoke.h"

#include "csrun/backends/vulkan/VulkanDevicePick.h"
#include "csrun/compiler/SpirvToolchain.h"
#include "csrun/core/Log.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <vector>

namespace csrun {
namespace {

Error vkError(const std::string &message, VkResult result) {
  Error e = Error::make(ErrorCode::DriverError,
                        message + " (VkResult=" + std::to_string(static_cast<int>(result)) + ")");
  e.backend = "vulkan";
  e.operation = "execute";
  return e;
}

Error unsupported(const std::string &message) {
  Error e = Error::make(ErrorCode::UnsupportedOperation, message);
  e.backend = "vulkan";
  e.operation = "execute";
  return e;
}

uint32_t alignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t bits,
                        VkMemoryPropertyFlags wanted) {
  VkPhysicalDeviceMemoryProperties properties{};
  vkGetPhysicalDeviceMemoryProperties(physical, &properties);
  for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) &&
        (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
      return i;
  }
  return std::numeric_limits<uint32_t>::max();
}

struct Buffer {
  VkBuffer handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
};

struct AccelerationStructure {
  VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
  Buffer storage;
  VkDeviceAddress address = 0;
};

struct Functions {
  PFN_vkGetBufferDeviceAddressKHR getBufferAddress = nullptr;
  PFN_vkCreateAccelerationStructureKHR createAs = nullptr;
  PFN_vkDestroyAccelerationStructureKHR destroyAs = nullptr;
  PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes = nullptr;
  PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild = nullptr;
  PFN_vkGetAccelerationStructureDeviceAddressKHR getAsAddress = nullptr;
  PFN_vkCreateRayTracingPipelinesKHR createPipelines = nullptr;
  PFN_vkGetRayTracingShaderGroupHandlesKHR getGroupHandles = nullptr;
  PFN_vkCmdTraceRaysKHR cmdTraceRays = nullptr;
};

VkDeviceAddress bufferAddress(VkDevice device, const Functions &fn, VkBuffer buffer) {
  VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = buffer;
  return fn.getBufferAddress(device, &info);
}

Expected<Buffer> createBuffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                              VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                              bool deviceAddress, const void *initial = nullptr) {
  Buffer out;
  out.size = size;
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult r = vkCreateBuffer(device, &ci, nullptr, &out.handle);
  if (r != VK_SUCCESS)
    return vkError("vkCreateBuffer failed", r);

  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, out.handle, &requirements);
  const uint32_t memoryType = findMemoryType(physical, requirements.memoryTypeBits, properties);
  if (memoryType == std::numeric_limits<uint32_t>::max()) {
    vkDestroyBuffer(device, out.handle, nullptr);
    return Error::make(ErrorCode::ResourceAllocationFailed, "no suitable Vulkan memory type");
  }

  VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = deviceAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.pNext = deviceAddress ? &flags : nullptr;
  ai.allocationSize = requirements.size;
  ai.memoryTypeIndex = memoryType;
  r = vkAllocateMemory(device, &ai, nullptr, &out.memory);
  if (r != VK_SUCCESS) {
    vkDestroyBuffer(device, out.handle, nullptr);
    return vkError("vkAllocateMemory failed", r);
  }
  r = vkBindBufferMemory(device, out.handle, out.memory, 0);
  if (r != VK_SUCCESS) {
    vkFreeMemory(device, out.memory, nullptr);
    vkDestroyBuffer(device, out.handle, nullptr);
    return vkError("vkBindBufferMemory failed", r);
  }
  if (initial) {
    void *mapped = nullptr;
    r = vkMapMemory(device, out.memory, 0, size, 0, &mapped);
    if (r != VK_SUCCESS) {
      vkFreeMemory(device, out.memory, nullptr);
      vkDestroyBuffer(device, out.handle, nullptr);
      return vkError("vkMapMemory failed", r);
    }
    std::memcpy(mapped, initial, static_cast<size_t>(size));
    vkUnmapMemory(device, out.memory);
  }
  return out;
}

void destroyBuffer(VkDevice device, Buffer &buffer) {
  if (buffer.handle)
    vkDestroyBuffer(device, buffer.handle, nullptr);
  if (buffer.memory)
    vkFreeMemory(device, buffer.memory, nullptr);
  buffer = {};
}

Expected<VkShaderModule> createShader(VkDevice device, const std::filesystem::path &path) {
  auto words = SpirvToolchain::loadSpvFile(path);
  if (!words)
    return words.error();
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = words.value().size() * sizeof(uint32_t);
  ci.pCode = words.value().data();
  VkShaderModule module = VK_NULL_HANDLE;
  const VkResult r = vkCreateShaderModule(device, &ci, nullptr, &module);
  if (r != VK_SUCCESS)
    return vkError("vkCreateShaderModule failed for " + path.string(), r);
  return module;
}

} // namespace

Expected<VulkanSmokeResult> vulkanRayTracingSmoke(const VulkanRayTracingRequest &req) {
  VulkanSmokeResult out;
  out.mode = "ray_tracing_offscreen";
  auto &steps = out.steps;
  const uint32_t width = std::max(1u, req.width);
  const uint32_t height = std::max(1u, req.height);

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "csrun-ray-tracing-smoke";
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo instanceCi{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instanceCi.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&instanceCi, nullptr, &instance);
  if (r != VK_SUCCESS)
    return vkError("vkCreateInstance failed", r);

  uint32_t physicalCount = 0;
  r = vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr);
  if (r != VK_SUCCESS || physicalCount == 0) {
    vkDestroyInstance(instance, nullptr);
    return r == VK_SUCCESS ? unsupported("no Vulkan physical devices")
                           : vkError("vkEnumeratePhysicalDevices failed", r);
  }
  std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
  vkEnumeratePhysicalDevices(instance, &physicalCount, physicalDevices.data());
  if (req.deviceIndex && *req.deviceIndex >= physicalCount) {
    vkDestroyInstance(instance, nullptr);
    return Error::make(ErrorCode::DeviceNotFound, "device index out of range");
  }
  const uint32_t physicalIndex =
      vulkanPickPhysicalDevice(physicalDevices, req.deviceIndex, true);
  VkPhysicalDevice physical = physicalDevices[physicalIndex];
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physical, &properties);
  out.deviceName = properties.deviceName;
  steps.push_back("selected device[" + std::to_string(physicalIndex) + "]: " + out.deviceName);

  uint32_t queueCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, nullptr);
  std::vector<VkQueueFamilyProperties> queueProperties(queueCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, queueProperties.data());
  uint32_t queueFamily = std::numeric_limits<uint32_t>::max();
  for (uint32_t i = 0; i < queueCount; ++i) {
    if (queueProperties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      queueFamily = i;
      break;
    }
  }
  if (queueFamily == std::numeric_limits<uint32_t>::max()) {
    vkDestroyInstance(instance, nullptr);
    return unsupported("no graphics-capable queue family for ray tracing");
  }

  static constexpr const char *requiredExtensions[] = {
      VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
      VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
      VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
      VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
      VK_KHR_SPIRV_1_4_EXTENSION_NAME,
      VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
  };
  uint32_t extensionCount = 0;
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr);
  std::vector<VkExtensionProperties> extensions(extensionCount);
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, extensions.data());
  for (const char *required : requiredExtensions) {
    const bool found = std::any_of(extensions.begin(), extensions.end(), [&](const auto &ext) {
      return std::strcmp(ext.extensionName, required) == 0;
    });
    if (!found) {
      vkDestroyInstance(instance, nullptr);
      return unsupported(std::string("device missing required ray tracing extension ") + required);
    }
  }

  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingFeatures{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddressFeatures{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
  rayTracingFeatures.pNext = &accelerationFeatures;
  accelerationFeatures.pNext = &bufferAddressFeatures;
  VkPhysicalDeviceFeatures2 featureQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  featureQuery.pNext = &rayTracingFeatures;
  vkGetPhysicalDeviceFeatures2(physical, &featureQuery);
  if (!rayTracingFeatures.rayTracingPipeline || !accelerationFeatures.accelerationStructure ||
      !bufferAddressFeatures.bufferDeviceAddress) {
    vkDestroyInstance(instance, nullptr);
    return unsupported("device does not expose required ray tracing feature bits");
  }
  rayTracingFeatures = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  rayTracingFeatures.rayTracingPipeline = VK_TRUE;
  accelerationFeatures = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  accelerationFeatures.accelerationStructure = VK_TRUE;
  bufferAddressFeatures = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
  bufferAddressFeatures.bufferDeviceAddress = VK_TRUE;
  rayTracingFeatures.pNext = &accelerationFeatures;
  accelerationFeatures.pNext = &bufferAddressFeatures;
  float priority = 1.0f;
  VkDeviceQueueCreateInfo queueCi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queueCi.queueFamilyIndex = queueFamily;
  queueCi.queueCount = 1;
  queueCi.pQueuePriorities = &priority;
  VkDeviceCreateInfo deviceCi{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  deviceCi.pNext = &rayTracingFeatures;
  deviceCi.queueCreateInfoCount = 1;
  deviceCi.pQueueCreateInfos = &queueCi;
  deviceCi.enabledExtensionCount =
      static_cast<uint32_t>(std::size(requiredExtensions));
  deviceCi.ppEnabledExtensionNames = requiredExtensions;
  VkDevice device = VK_NULL_HANDLE;
  r = vkCreateDevice(physical, &deviceCi, nullptr, &device);
  if (r != VK_SUCCESS) {
    vkDestroyInstance(instance, nullptr);
    return vkError("vkCreateDevice with ray tracing extensions failed", r);
  }
  steps.push_back("enabled acceleration_structure + ray_tracing_pipeline + buffer_device_address");

  Functions fn;
  fn.getBufferAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
      vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddressKHR"));
  fn.createAs = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
      vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
  fn.destroyAs = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
      vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
  fn.getBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
      vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
  fn.cmdBuild = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
      vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
  fn.getAsAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
      vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
  fn.createPipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
      vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR"));
  fn.getGroupHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
      vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesKHR"));
  fn.cmdTraceRays = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
      vkGetDeviceProcAddr(device, "vkCmdTraceRaysKHR"));
  if (!fn.getBufferAddress || !fn.createAs || !fn.destroyAs || !fn.getBuildSizes ||
      !fn.cmdBuild || !fn.getAsAddress || !fn.createPipelines || !fn.getGroupHandles ||
      !fn.cmdTraceRays) {
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return unsupported("required Vulkan ray tracing entry points are unavailable");
  }

  VkQueue queue = VK_NULL_HANDLE;
  vkGetDeviceQueue(device, queueFamily, 0, &queue);
  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolCi.queueFamilyIndex = queueFamily;
  poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  r = vkCreateCommandPool(device, &poolCi, nullptr, &commandPool);
  if (r != VK_SUCCESS) {
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return vkError("vkCreateCommandPool failed", r);
  }
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo commandAi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  commandAi.commandPool = commandPool;
  commandAi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  commandAi.commandBufferCount = 1;
  r = vkAllocateCommandBuffers(device, &commandAi, &command);
  if (r != VK_SUCCESS) {
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return vkError("vkAllocateCommandBuffers failed", r);
  }

  Buffer vertex, index, instanceBuffer, blasScratch, tlasScratch, sbt, camera, staging;
  AccelerationStructure blas, tlas;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory imageMemory = VK_NULL_HANDLE;
  VkImageView imageView = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkShaderModule raygen = VK_NULL_HANDLE, miss = VK_NULL_HANDLE, closestHit = VK_NULL_HANDLE;

  auto cleanup = [&]() {
    if (device)
      vkDeviceWaitIdle(device);
    if (pipeline)
      vkDestroyPipeline(device, pipeline, nullptr);
    if (pipelineLayout)
      vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (descriptorPool)
      vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    if (descriptorLayout)
      vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
    if (raygen)
      vkDestroyShaderModule(device, raygen, nullptr);
    if (miss)
      vkDestroyShaderModule(device, miss, nullptr);
    if (closestHit)
      vkDestroyShaderModule(device, closestHit, nullptr);
    if (imageView)
      vkDestroyImageView(device, imageView, nullptr);
    if (image)
      vkDestroyImage(device, image, nullptr);
    if (imageMemory)
      vkFreeMemory(device, imageMemory, nullptr);
    if (tlas.handle)
      fn.destroyAs(device, tlas.handle, nullptr);
    if (blas.handle)
      fn.destroyAs(device, blas.handle, nullptr);
    destroyBuffer(device, tlas.storage);
    destroyBuffer(device, blas.storage);
    destroyBuffer(device, vertex);
    destroyBuffer(device, index);
    destroyBuffer(device, instanceBuffer);
    destroyBuffer(device, blasScratch);
    destroyBuffer(device, tlasScratch);
    destroyBuffer(device, sbt);
    destroyBuffer(device, camera);
    destroyBuffer(device, staging);
    if (commandPool)
      vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
  };
  auto fail = [&](const Error &error) -> Expected<VulkanSmokeResult> {
    cleanup();
    return error;
  };
  auto submitCommand = [&]() -> Expected<void> {
    r = vkEndCommandBuffer(command);
    if (r != VK_SUCCESS)
      return vkError("vkEndCommandBuffer failed", r);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    r = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    if (r != VK_SUCCESS)
      return vkError("vkQueueSubmit failed", r);
    r = vkQueueWaitIdle(queue);
    if (r != VK_SUCCESS)
      return vkError("vkQueueWaitIdle failed", r);
    return {};
  };
  auto beginCommand = [&]() -> Expected<void> {
    r = vkResetCommandBuffer(command, 0);
    if (r != VK_SUCCESS)
      return vkError("vkResetCommandBuffer failed", r);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(command, &begin);
    if (r != VK_SUCCESS)
      return vkError("vkBeginCommandBuffer failed", r);
    return {};
  };

  struct Vertex {
    float x, y, z;
  };
  const Vertex vertices[] = {{1.0f, 1.0f, 0.0f},
                             {-1.0f, 1.0f, 0.0f},
                             {0.0f, -1.0f, 0.0f}};
  const uint32_t indices[] = {0, 1, 2};
  constexpr VkBufferUsageFlags buildInputUsage =
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  auto vertexOr = createBuffer(physical, device, sizeof(vertices), buildInputUsage,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               true, vertices);
  if (!vertexOr)
    return fail(vertexOr.error());
  vertex = vertexOr.value();
  auto indexOr = createBuffer(physical, device, sizeof(indices), buildInputUsage,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              true, indices);
  if (!indexOr)
    return fail(indexOr.error());
  index = indexOr.value();

  VkAccelerationStructureGeometryKHR blasGeometry{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  blasGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  blasGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
  auto &triangles = blasGeometry.geometry.triangles;
  triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  triangles.vertexData.deviceAddress = bufferAddress(device, fn, vertex.handle);
  triangles.vertexStride = sizeof(Vertex);
  triangles.maxVertex = 2;
  triangles.indexType = VK_INDEX_TYPE_UINT32;
  triangles.indexData.deviceAddress = bufferAddress(device, fn, index.handle);
  VkAccelerationStructureBuildGeometryInfoKHR blasBuild{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  blasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  blasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  blasBuild.geometryCount = 1;
  blasBuild.pGeometries = &blasGeometry;
  const uint32_t primitiveCount = 1;
  VkAccelerationStructureBuildSizesInfoKHR blasSizes{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  fn.getBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &blasBuild,
                   &primitiveCount, &blasSizes);
  auto blasStorageOr = createBuffer(
      physical, device, blasSizes.accelerationStructureSize,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
  if (!blasStorageOr)
    return fail(blasStorageOr.error());
  blas.storage = blasStorageOr.value();
  VkAccelerationStructureCreateInfoKHR blasCi{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  blasCi.buffer = blas.storage.handle;
  blasCi.size = blasSizes.accelerationStructureSize;
  blasCi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  r = fn.createAs(device, &blasCi, nullptr, &blas.handle);
  if (r != VK_SUCCESS)
    return fail(vkError("vkCreateAccelerationStructureKHR (BLAS) failed", r));
  auto blasScratchOr = createBuffer(
      physical, device, blasSizes.buildScratchSize,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
  if (!blasScratchOr)
    return fail(blasScratchOr.error());
  blasScratch = blasScratchOr.value();
  blasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  blasBuild.dstAccelerationStructure = blas.handle;
  blasBuild.scratchData.deviceAddress = bufferAddress(device, fn, blasScratch.handle);
  VkAccelerationStructureBuildRangeInfoKHR blasRange{};
  blasRange.primitiveCount = 1;
  const VkAccelerationStructureBuildRangeInfoKHR *blasRanges[] = {&blasRange};
  if (auto begun = beginCommand(); !begun)
    return fail(begun.error());
  fn.cmdBuild(command, 1, &blasBuild, blasRanges);
  if (auto submitted = submitCommand(); !submitted)
    return fail(submitted.error());
  VkAccelerationStructureDeviceAddressInfoKHR blasAddressInfo{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  blasAddressInfo.accelerationStructure = blas.handle;
  blas.address = fn.getAsAddress(device, &blasAddressInfo);
  steps.push_back("built triangle BLAS");

  VkAccelerationStructureInstanceKHR asInstance{};
  asInstance.transform.matrix[0][0] = 1.0f;
  asInstance.transform.matrix[1][1] = 1.0f;
  asInstance.transform.matrix[2][2] = 1.0f;
  asInstance.mask = 0xff;
  asInstance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
  asInstance.accelerationStructureReference = blas.address;
  auto instanceOr = createBuffer(physical, device, sizeof(asInstance), buildInputUsage,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 true, &asInstance);
  if (!instanceOr)
    return fail(instanceOr.error());
  instanceBuffer = instanceOr.value();
  VkAccelerationStructureGeometryKHR tlasGeometry{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  tlasGeometry.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  tlasGeometry.geometry.instances.data.deviceAddress =
      bufferAddress(device, fn, instanceBuffer.handle);
  VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  tlasBuild.geometryCount = 1;
  tlasBuild.pGeometries = &tlasGeometry;
  VkAccelerationStructureBuildSizesInfoKHR tlasSizes{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  fn.getBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild,
                   &primitiveCount, &tlasSizes);
  auto tlasStorageOr = createBuffer(
      physical, device, tlasSizes.accelerationStructureSize,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
  if (!tlasStorageOr)
    return fail(tlasStorageOr.error());
  tlas.storage = tlasStorageOr.value();
  VkAccelerationStructureCreateInfoKHR tlasCi{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  tlasCi.buffer = tlas.storage.handle;
  tlasCi.size = tlasSizes.accelerationStructureSize;
  tlasCi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  r = fn.createAs(device, &tlasCi, nullptr, &tlas.handle);
  if (r != VK_SUCCESS)
    return fail(vkError("vkCreateAccelerationStructureKHR (TLAS) failed", r));
  auto tlasScratchOr = createBuffer(
      physical, device, tlasSizes.buildScratchSize,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
  if (!tlasScratchOr)
    return fail(tlasScratchOr.error());
  tlasScratch = tlasScratchOr.value();
  tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  tlasBuild.dstAccelerationStructure = tlas.handle;
  tlasBuild.scratchData.deviceAddress = bufferAddress(device, fn, tlasScratch.handle);
  VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
  tlasRange.primitiveCount = 1;
  const VkAccelerationStructureBuildRangeInfoKHR *tlasRanges[] = {&tlasRange};
  if (auto begun = beginCommand(); !begun)
    return fail(begun.error());
  fn.cmdBuild(command, 1, &tlasBuild, tlasRanges);
  if (auto submitted = submitCommand(); !submitted)
    return fail(submitted.error());
  steps.push_back("built one-instance TLAS");

  VkImageCreateInfo imageCi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imageCi.imageType = VK_IMAGE_TYPE_2D;
  imageCi.format = VK_FORMAT_R8G8B8A8_UNORM;
  imageCi.extent = {width, height, 1};
  imageCi.mipLevels = 1;
  imageCi.arrayLayers = 1;
  imageCi.samples = VK_SAMPLE_COUNT_1_BIT;
  imageCi.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageCi.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  r = vkCreateImage(device, &imageCi, nullptr, &image);
  if (r != VK_SUCCESS)
    return fail(vkError("vkCreateImage (ray tracing output) failed", r));
  VkMemoryRequirements imageRequirements{};
  vkGetImageMemoryRequirements(device, image, &imageRequirements);
  const uint32_t imageMemoryType =
      findMemoryType(physical, imageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (imageMemoryType == std::numeric_limits<uint32_t>::max())
    return fail(Error::make(ErrorCode::ResourceAllocationFailed,
                            "no device-local memory for ray tracing output"));
  VkMemoryAllocateInfo imageAi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  imageAi.allocationSize = imageRequirements.size;
  imageAi.memoryTypeIndex = imageMemoryType;
  r = vkAllocateMemory(device, &imageAi, nullptr, &imageMemory);
  if (r != VK_SUCCESS)
    return fail(vkError("vkAllocateMemory (ray tracing output) failed", r));
  r = vkBindImageMemory(device, image, imageMemory, 0);
  if (r != VK_SUCCESS)
    return fail(vkError("vkBindImageMemory failed", r));
  VkImageViewCreateInfo viewCi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  viewCi.image = image;
  viewCi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewCi.format = VK_FORMAT_R8G8B8A8_UNORM;
  viewCi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewCi.subresourceRange.levelCount = 1;
  viewCi.subresourceRange.layerCount = 1;
  r = vkCreateImageView(device, &viewCi, nullptr, &imageView);
  if (r != VK_SUCCESS)
    return fail(vkError("vkCreateImageView failed", r));

  struct Camera {
    float viewInverse[16];
    float projInverse[16];
  } cameraData{};
  for (uint32_t i = 0; i < 4; ++i) {
    cameraData.viewInverse[i * 5] = 1.0f;
    cameraData.projInverse[i * 5] = 1.0f;
  }
  auto cameraOr = createBuffer(physical, device, sizeof(cameraData),
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               false, &cameraData);
  if (!cameraOr)
    return fail(cameraOr.error());
  camera = cameraOr.value();
  const VkDeviceSize stagingBytes = static_cast<VkDeviceSize>(width) * height * 4;
  auto stagingOr = createBuffer(physical, device, stagingBytes,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                false);
  if (!stagingOr)
    return fail(stagingOr.error());
  staging = stagingOr.value();

  VkDescriptorSetLayoutBinding bindings[3]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
  bindings[2].binding = 2;
  bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  bindings[2].descriptorCount = 1;
  bindings[2].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
  VkDescriptorSetLayoutCreateInfo layoutCi{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layoutCi.bindingCount = 3;
  layoutCi.pBindings = bindings;
  r = vkCreateDescriptorSetLayout(device, &layoutCi, nullptr, &descriptorLayout);
  if (r != VK_SUCCESS)
    return fail(vkError("vkCreateDescriptorSetLayout failed", r));
  VkPipelineLayoutCreateInfo pipelineLayoutCi{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipelineLayoutCi.setLayoutCount = 1;
  pipelineLayoutCi.pSetLayouts = &descriptorLayout;
  r = vkCreatePipelineLayout(device, &pipelineLayoutCi, nullptr, &pipelineLayout);
  if (r != VK_SUCCESS)
    return fail(vkError("vkCreatePipelineLayout failed", r));

  auto raygenOr = createShader(device, req.raygenSpv);
  if (!raygenOr)
    return fail(raygenOr.error());
  raygen = raygenOr.value();
  auto missOr = createShader(device, req.missSpv);
  if (!missOr)
    return fail(missOr.error());
  miss = missOr.value();
  auto hitOr = createShader(device, req.closestHitSpv);
  if (!hitOr)
    return fail(hitOr.error());
  closestHit = hitOr.value();
  VkPipelineShaderStageCreateInfo stages[3]{};
  const VkShaderStageFlagBits stageFlags[3] = {
      VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
      VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
  const VkShaderModule modules[3] = {raygen, miss, closestHit};
  for (uint32_t i = 0; i < 3; ++i) {
    stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[i].stage = stageFlags[i];
    stages[i].module = modules[i];
    stages[i].pName = "main";
  }
  VkRayTracingShaderGroupCreateInfoKHR groups[3]{};
  for (auto &group : groups) {
    group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    group.generalShader = VK_SHADER_UNUSED_KHR;
    group.closestHitShader = VK_SHADER_UNUSED_KHR;
    group.anyHitShader = VK_SHADER_UNUSED_KHR;
    group.intersectionShader = VK_SHADER_UNUSED_KHR;
  }
  groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  groups[0].generalShader = 0;
  groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  groups[1].generalShader = 1;
  groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  groups[2].closestHitShader = 2;
  VkRayTracingPipelineCreateInfoKHR pipelineCi{
      VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
  pipelineCi.stageCount = 3;
  pipelineCi.pStages = stages;
  pipelineCi.groupCount = 3;
  pipelineCi.pGroups = groups;
  pipelineCi.maxPipelineRayRecursionDepth = 1;
  pipelineCi.layout = pipelineLayout;
  r = fn.createPipelines(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineCi, nullptr,
                         &pipeline);
  if (r != VK_SUCCESS)
    return fail(vkError("vkCreateRayTracingPipelinesKHR failed", r));
  steps.push_back("created raygen + miss + closest-hit pipeline");

  VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayProperties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
  VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties2.pNext = &rayProperties;
  vkGetPhysicalDeviceProperties2(physical, &properties2);
  const uint32_t handleSize = rayProperties.shaderGroupHandleSize;
  const uint32_t recordStride =
      alignUp(handleSize, rayProperties.shaderGroupHandleAlignment);
  const uint32_t recordSlot =
      alignUp(recordStride, rayProperties.shaderGroupBaseAlignment);
  std::vector<uint8_t> handles(static_cast<size_t>(handleSize) * 3);
  r = fn.getGroupHandles(device, pipeline, 0, 3, handles.size(), handles.data());
  if (r != VK_SUCCESS)
    return fail(vkError("vkGetRayTracingShaderGroupHandlesKHR failed", r));
  auto sbtOr = createBuffer(
      physical, device, static_cast<VkDeviceSize>(recordSlot) * 3,
      VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
  if (!sbtOr)
    return fail(sbtOr.error());
  sbt = sbtOr.value();
  void *sbtMapped = nullptr;
  r = vkMapMemory(device, sbt.memory, 0, sbt.size, 0, &sbtMapped);
  if (r != VK_SUCCESS)
    return fail(vkError("vkMapMemory (SBT) failed", r));
  std::memset(sbtMapped, 0, static_cast<size_t>(sbt.size));
  auto *sbtBytes = static_cast<uint8_t *>(sbtMapped);
  for (uint32_t i = 0; i < 3; ++i)
    std::memcpy(sbtBytes + static_cast<size_t>(i) * recordSlot,
                handles.data() + static_cast<size_t>(i) * handleSize, handleSize);
  vkUnmapMemory(device, sbt.memory);
  const VkDeviceAddress sbtAddress = bufferAddress(device, fn, sbt.handle);
  VkStridedDeviceAddressRegionKHR raygenRegion{sbtAddress, recordStride, recordStride};
  VkStridedDeviceAddressRegionKHR missRegion{sbtAddress + recordSlot, recordStride,
                                              recordStride};
  VkStridedDeviceAddressRegionKHR hitRegion{sbtAddress + recordSlot * 2, recordStride,
                                             recordStride};
  VkStridedDeviceAddressRegionKHR callableRegion{};
  steps.push_back("created shader binding table");

  VkDescriptorPoolSize poolSizes[3] = {
      {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
  };
  VkDescriptorPoolCreateInfo descriptorPoolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  descriptorPoolCi.maxSets = 1;
  descriptorPoolCi.poolSizeCount = 3;
  descriptorPoolCi.pPoolSizes = poolSizes;
  r = vkCreateDescriptorPool(device, &descriptorPoolCi, nullptr, &descriptorPool);
  if (r != VK_SUCCESS)
    return fail(vkError("vkCreateDescriptorPool failed", r));
  VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo descriptorAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  descriptorAi.descriptorPool = descriptorPool;
  descriptorAi.descriptorSetCount = 1;
  descriptorAi.pSetLayouts = &descriptorLayout;
  r = vkAllocateDescriptorSets(device, &descriptorAi, &descriptorSet);
  if (r != VK_SUCCESS)
    return fail(vkError("vkAllocateDescriptorSets failed", r));
  VkWriteDescriptorSetAccelerationStructureKHR asWrite{
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  asWrite.accelerationStructureCount = 1;
  asWrite.pAccelerationStructures = &tlas.handle;
  VkDescriptorImageInfo imageInfo{};
  imageInfo.imageView = imageView;
  imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorBufferInfo cameraInfo{};
  cameraInfo.buffer = camera.handle;
  cameraInfo.range = sizeof(cameraData);
  VkWriteDescriptorSet descriptorWrites[3]{};
  descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrites[0].pNext = &asWrite;
  descriptorWrites[0].dstSet = descriptorSet;
  descriptorWrites[0].dstBinding = 0;
  descriptorWrites[0].descriptorCount = 1;
  descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrites[1].dstSet = descriptorSet;
  descriptorWrites[1].dstBinding = 1;
  descriptorWrites[1].descriptorCount = 1;
  descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptorWrites[1].pImageInfo = &imageInfo;
  descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrites[2].dstSet = descriptorSet;
  descriptorWrites[2].dstBinding = 2;
  descriptorWrites[2].descriptorCount = 1;
  descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptorWrites[2].pBufferInfo = &cameraInfo;
  vkUpdateDescriptorSets(device, 3, descriptorWrites, 0, nullptr);

  if (auto begun = beginCommand(); !begun)
    return fail(begun.error());
  VkImageMemoryBarrier initializeImage{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  initializeImage.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  initializeImage.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  initializeImage.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  initializeImage.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  initializeImage.image = image;
  initializeImage.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  initializeImage.subresourceRange.levelCount = 1;
  initializeImage.subresourceRange.layerCount = 1;
  initializeImage.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0,
                       nullptr, 1, &initializeImage);
  if (auto submitted = submitCommand(); !submitted)
    return fail(submitted.error());

  const int warmup = std::max(0, req.warmup);
  const int iterations = std::max(1, req.iterations);
  for (int iteration = -warmup; iteration < iterations; ++iteration) {
    if (auto begun = beginCommand(); !begun)
      return fail(begun.error());
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineLayout,
                            0, 1, &descriptorSet, 0, nullptr);
    fn.cmdTraceRays(command, &raygenRegion, &missRegion, &hitRegion, &callableRegion, width,
                    height, 1);
    VkImageMemoryBarrier toCopy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toCopy.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopy.image = image;
    toCopy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toCopy.subresourceRange.levelCount = 1;
    toCopy.subresourceRange.layerCount = 1;
    toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toCopy);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging.handle, 1, &copy);
    VkImageMemoryBarrier backToGeneral = toCopy;
    backToGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    backToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    backToGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    backToGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0,
                         nullptr, 1, &backToGeneral);
    const auto start = std::chrono::steady_clock::now();
    if (auto submitted = submitCommand(); !submitted)
      return fail(submitted.error());
    const auto end = std::chrono::steady_clock::now();
    if (iteration >= 0)
      out.samplesMs.push_back(
          std::chrono::duration<double, std::milli>(end - start).count());
  }
  out.hostMs = out.samplesMs.empty() ? 0.0 : out.samplesMs.back();
  out.timingSource = TimingSource::CpuWall;
  out.executed = true;
  steps.push_back("vkCmdTraceRaysKHR " + std::to_string(width) + "x" +
                  std::to_string(height) + " iterations=" + std::to_string(iterations));

  void *pixelsMapped = nullptr;
  r = vkMapMemory(device, staging.memory, 0, stagingBytes, 0, &pixelsMapped);
  if (r != VK_SUCCESS)
    return fail(vkError("vkMapMemory (ray tracing readback) failed", r));
  const auto *pixels = static_cast<const uint8_t *>(pixelsMapped);
  const size_t centerOffset =
      (static_cast<size_t>(height / 2) * width + width / 2) * 4;
  out.centerR = pixels[centerOffset];
  out.centerG = pixels[centerOffset + 1];
  out.centerB = pixels[centerOffset + 2];
  out.centerA = pixels[centerOffset + 3];
  out.pixelChecked = true;
  out.ok = (out.centerR | out.centerG | out.centerB) != 0;
  if (!out.ok) {
    for (size_t offset = 0; offset + 3 < static_cast<size_t>(stagingBytes); offset += 4) {
      if ((pixels[offset] | pixels[offset + 1] | pixels[offset + 2]) != 0) {
        out.ok = true;
        steps.push_back("non-black ray tracing pixel found off-center");
        break;
      }
    }
  }
  vkUnmapMemory(device, staging.memory);
  steps.push_back("readback center pixel rgba=(" + std::to_string(out.centerR) + "," +
                  std::to_string(out.centerG) + "," + std::to_string(out.centerB) + "," +
                  std::to_string(out.centerA) + ")");
  if (!out.ok)
    steps.push_back("FAIL: ray tracing output is black");

  cleanup();
  logInfo("runtime", "Vulkan ray tracing smoke " + std::string(out.ok ? "OK" : "FAIL") +
                         " on " + out.deviceName);
  return out;
}

} // namespace csrun
