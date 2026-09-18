#include "csrun/backends/vulkan/VulkanSpirvRunner.h"

#include "csrun/core/Log.h"

#include <vulkan/vulkan.h>

#include <cstring>
#include <limits>
#include <vector>

namespace csrun {
namespace {

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

} // namespace

Expected<VulkanSmokeResult> vulkanSmokeSpirv(const VulkanSmokeRequest &req) {
  const bool wantCompute = !req.computeSpv.empty();
  const bool wantGraphics = !req.vertexSpv.empty() && !req.fragmentSpv.empty();
  if (!wantCompute && !wantGraphics) {
    return Error::make(ErrorCode::InvalidWorkload,
                       "provide --spv (compute) or --vert-spv + --frag-spv (graphics)");
  }

  VulkanSmokeResult out;
  auto &steps = out.steps;

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "csrun-spirv-smoke";
  app.apiVersion = VK_API_VERSION_1_1;

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
  steps.push_back("vkEnumeratePhysicalDevices");

  if (req.deviceIndex >= physCount) {
    vkDestroyInstance(instance, nullptr);
    return Error::make(ErrorCode::DeviceNotFound, "device index out of range");
  }
  VkPhysicalDevice physical = phys[req.deviceIndex];
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physical, &props);
  out.deviceName = props.deviceName;
  steps.push_back(std::string("selected device: ") + props.deviceName);

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

  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VkDevice device = VK_NULL_HANDLE;
  r = vkCreateDevice(physical, &dci, nullptr, &device);
  if (r != VK_SUCCESS) {
    vkDestroyInstance(instance, nullptr);
    return vkError("vkCreateDevice failed", r);
  }
  steps.push_back("vkCreateDevice");

  auto cleanup = [&]() {
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
  };

  if (wantCompute) {
    out.mode = "compute_module_pipeline";
    auto words = loadWords(req.computeSpv);
    if (!words) {
      cleanup();
      return words.error();
    }
    steps.push_back("loaded compute SPIR-V: " + req.computeSpv.string());

    auto mod = createModule(device, words.value(), steps);
    if (!mod) {
      cleanup();
      return mod.error();
    }

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
    steps.push_back("vkCreateDescriptorSetLayout");

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = 0;
    push.size = sizeof(uint32_t);

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

    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    vkDestroyShaderModule(device, mod.value(), nullptr);
    steps.push_back("cleanup compute objects");
  } else {
    out.mode = "graphics_module_pipeline";
    auto vWords = loadWords(req.vertexSpv);
    auto fWords = loadWords(req.fragmentSpv);
    if (!vWords) {
      cleanup();
      return vWords.error();
    }
    if (!fWords) {
      cleanup();
      return fWords.error();
    }
    steps.push_back("loaded vertex SPIR-V: " + req.vertexSpv.string());
    steps.push_back("loaded fragment SPIR-V: " + req.fragmentSpv.string());

    auto vMod = createModule(device, vWords.value(), steps);
    if (!vMod) {
      cleanup();
      return vMod.error();
    }
    auto fMod = createModule(device, fWords.value(), steps);
    if (!fMod) {
      vkDestroyShaderModule(device, vMod.value(), nullptr);
      cleanup();
      return fMod.error();
    }

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    r = vkCreatePipelineLayout(device, &plci, nullptr, &layout);
    if (r != VK_SUCCESS) {
      vkDestroyShaderModule(device, fMod.value(), nullptr);
      vkDestroyShaderModule(device, vMod.value(), nullptr);
      cleanup();
      return vkError("vkCreatePipelineLayout failed", r);
    }
    steps.push_back("vkCreatePipelineLayout");

    VkAttachmentDescription colorAtt{};
    colorAtt.format = VK_FORMAT_R8G8B8A8_UNORM;
    colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAtt.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &colorAtt;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    r = vkCreateRenderPass(device, &rpci, nullptr, &renderPass);
    if (r != VK_SUCCESS) {
      vkDestroyPipelineLayout(device, layout, nullptr);
      vkDestroyShaderModule(device, fMod.value(), nullptr);
      vkDestroyShaderModule(device, vMod.value(), nullptr);
      cleanup();
      return vkError("vkCreateRenderPass failed", r);
    }
    steps.push_back("vkCreateRenderPass");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vMod.value();
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fMod.value();
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.width = 64.0f;
    viewport.height = 64.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {64, 64};

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

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blendAtt;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &cb;
    gpci.layout = layout;
    gpci.renderPass = renderPass;
    gpci.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
      vkDestroyRenderPass(device, renderPass, nullptr);
      vkDestroyPipelineLayout(device, layout, nullptr);
      vkDestroyShaderModule(device, fMod.value(), nullptr);
      vkDestroyShaderModule(device, vMod.value(), nullptr);
      cleanup();
      return vkError("vkCreateGraphicsPipelines failed", r);
    }
    steps.push_back("vkCreateGraphicsPipelines");

    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyShaderModule(device, fMod.value(), nullptr);
    vkDestroyShaderModule(device, vMod.value(), nullptr);
    steps.push_back("cleanup graphics objects");
  }

  cleanup();
  steps.push_back("vkDestroyDevice + vkDestroyInstance");
  out.ok = true;
  logInfo("runtime", "Vulkan SPIR-V smoke OK (" + out.mode + ") on " + out.deviceName);
  return out;
}

} // namespace csrun
