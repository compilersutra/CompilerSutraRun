#define VK_USE_PLATFORM_XLIB_KHR

#include "csrun/backends/vulkan/VulkanWsiPresenter.h"

#include "csrun/backends/vulkan/VulkanDevicePick.h"
#include "csrun/backends/vulkan/VulkanPerfQuery.h"
#include "csrun/backends/vulkan/VulkanPipelineQueries.h"
#include "csrun/compiler/SpirvToolchain.h"
#include "csrun/core/Log.h"

#include <vulkan/vulkan.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <chrono>
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

Expected<VkShaderModule> createModule(VkDevice device, const std::vector<uint32_t> &words) {
  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = words.size() * sizeof(uint32_t);
  ci.pCode = words.data();
  VkShaderModule mod = VK_NULL_HANDLE;
  VkResult r = vkCreateShaderModule(device, &ci, nullptr, &mod);
  if (r != VK_SUCCESS)
    return vkError("vkCreateShaderModule failed", r);
  return mod;
}

struct SwapchainSupport {
  VkSurfaceCapabilitiesKHR caps{};
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> modes;
};

SwapchainSupport querySwapchain(VkPhysicalDevice phys, VkSurfaceKHR surface) {
  SwapchainSupport s;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &s.caps);
  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, nullptr);
  s.formats.resize(n);
  if (n)
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, s.formats.data());
  n = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, nullptr);
  s.modes.resize(n);
  if (n)
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, s.modes.data());
  return s;
}

VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR> &formats) {
  for (const auto &f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return f;
  }
  for (const auto &f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
      return f;
  }
  return formats.front();
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR> &modes) {
  for (auto m : modes) {
    if (m == VK_PRESENT_MODE_MAILBOX_KHR)
      return m;
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR &caps, uint32_t w, uint32_t h) {
  if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
    return caps.currentExtent;
  VkExtent2D e{};
  e.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
  e.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
  return e;
}

} // namespace

Expected<VulkanWsiResult> vulkanPresentGraphics(const VulkanWsiRequest &req) {
  if (req.vertexSpv.empty() || req.fragmentSpv.empty()) {
    return Error::make(ErrorCode::InvalidWorkload,
                       "WSI present requires vertex + fragment SPIR-V");
  }

  VulkanWsiResult out;
  auto &steps = out.steps;

  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy)
    return Error::make(ErrorCode::DriverError,
                       "XOpenDisplay failed (set DISPLAY for WSI present)");

  const int screen = DefaultScreen(dpy);
  const Window root = RootWindow(dpy, screen);
  const unsigned long black = BlackPixel(dpy, screen);

  XSetWindowAttributes attrs{};
  attrs.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask;
  attrs.background_pixel = black;
  attrs.border_pixel = black;
  Window win =
      XCreateWindow(dpy, root, 64, 64, static_cast<unsigned>(req.width),
                    static_cast<unsigned>(req.height), 0, CopyFromParent, InputOutput,
                    CopyFromParent, CWEventMask | CWBackPixel | CWBorderPixel, &attrs);
  XStoreName(dpy, win, req.title.c_str());
  Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, win, &wmDelete, 1);
  XMapWindow(dpy, win);
  XFlush(dpy);
  steps.push_back("XCreateWindow " + std::to_string(req.width) + "x" +
                  std::to_string(req.height));

  const char *instanceExts[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                VK_KHR_XLIB_SURFACE_EXTENSION_NAME};

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "csrun-wsi";
  app.apiVersion = VK_API_VERSION_1_2;

  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = 2;
  ici.ppEnabledExtensionNames = instanceExts;

  VkInstance instance = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&ici, nullptr, &instance);
  if (r != VK_SUCCESS) {
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return vkError("vkCreateInstance (WSI) failed", r);
  }
  steps.push_back("vkCreateInstance (+ VK_KHR_surface, VK_KHR_xlib_surface)");

  VkXlibSurfaceCreateInfoKHR sciSurf{};
  sciSurf.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
  sciSurf.dpy = dpy;
  sciSurf.window = win;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  r = vkCreateXlibSurfaceKHR(instance, &sciSurf, nullptr, &surface);
  if (r != VK_SUCCESS) {
    vkDestroyInstance(instance, nullptr);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return vkError("vkCreateXlibSurfaceKHR failed", r);
  }
  steps.push_back("VkSurfaceKHR (Xlib) created");

  uint32_t physCount = 0;
  vkEnumeratePhysicalDevices(instance, &physCount, nullptr);
  if (physCount == 0) {
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return Error::make(ErrorCode::DeviceNotFound, "no Vulkan physical devices");
  }
  std::vector<VkPhysicalDevice> phys(physCount);
  vkEnumeratePhysicalDevices(instance, &physCount, phys.data());
  if (req.deviceIndex.has_value() && *req.deviceIndex >= physCount) {
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return Error::make(ErrorCode::DeviceNotFound, "device index out of range");
  }

  auto deviceCanPresent = [&](VkPhysicalDevice p, uint32_t &outGraphics,
                              uint32_t &outPresent) -> bool {
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(p, &qCount, qprops.data());
    outGraphics = outPresent = std::numeric_limits<uint32_t>::max();
    for (uint32_t i = 0; i < qCount; ++i) {
      if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        outGraphics = i;
      VkBool32 support = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(p, i, surface, &support);
      if (support)
        outPresent = i;
    }
    return outGraphics != std::numeric_limits<uint32_t>::max() &&
           outPresent != std::numeric_limits<uint32_t>::max();
  };

  VkPhysicalDevice physical = VK_NULL_HANDLE;
  uint32_t graphicsFamily = 0;
  uint32_t presentFamily = 0;
  VulkanDeviceMeta meta{};
  {
    int bestScore = std::numeric_limits<int>::min();
    const bool autoPick = !req.deviceIndex.has_value();
    for (uint32_t i = 0; i < physCount; ++i) {
      if (!autoPick && i != *req.deviceIndex)
        continue;
      uint32_t g = 0, pr = 0;
      if (!deviceCanPresent(phys[i], g, pr))
        continue;
      VulkanDeviceMeta m = vulkanQueryDeviceMeta(phys[i], i);
      const int s = vulkanDeviceScore(m);
      if (physical == VK_NULL_HANDLE || s > bestScore) {
        bestScore = s;
        physical = phys[i];
        graphicsFamily = g;
        presentFamily = pr;
        meta = std::move(m);
      }
      if (!autoPick)
        break;
    }
  }
  if (!physical) {
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return Error::make(ErrorCode::DeviceNotFound, "no device can present to this surface");
  }

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physical, &props);
  out.deviceName = props.deviceName;
  steps.push_back("selected present device: " + meta.name + " [" + meta.typeName +
                  "] driver=" + meta.driverName +
                  (meta.hasPerfQuery ? " perf_query=yes" : " perf_query=no"));

  const float prio = 1.0f;
  std::vector<VkDeviceQueueCreateInfo> qcis;
  {
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = graphicsFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    qcis.push_back(qci);
    if (presentFamily != graphicsFamily) {
      qci.queueFamilyIndex = presentFamily;
      qcis.push_back(qci);
    }
  }

  const VulkanPerfDeviceSetup perfSetup = vulkanPreparePerfQueryDevice(physical);
  std::vector<const char *> deviceExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  if (perfSetup.want)
    deviceExts.push_back(VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME);

  VkPhysicalDeviceFeatures supportedFeatures{};
  vkGetPhysicalDeviceFeatures(physical, &supportedFeatures);
  const bool pipeStatsSupported = supportedFeatures.pipelineStatisticsQuery == VK_TRUE;
  VkPhysicalDeviceFeatures enabledFeatures{};
  if (pipeStatsSupported)
    enabledFeatures.pipelineStatisticsQuery = VK_TRUE;

  VkPhysicalDevicePerformanceQueryFeaturesKHR enabledPerfFeatures = perfSetup.features;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.features = enabledFeatures;

  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
  dci.pQueueCreateInfos = qcis.data();
  dci.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
  dci.ppEnabledExtensionNames = deviceExts.data();
  if (perfSetup.want) {
    enabledPerfFeatures.pNext = nullptr;
    features2.pNext = &enabledPerfFeatures;
    dci.pNext = &features2;
    dci.pEnabledFeatures = nullptr;
  } else {
    dci.pEnabledFeatures = &enabledFeatures;
  }

  VkDevice device = VK_NULL_HANDLE;
  r = vkCreateDevice(physical, &dci, nullptr, &device);
  if (r != VK_SUCCESS) {
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return vkError("vkCreateDevice (swapchain) failed", r);
  }
  {
    std::string devNote = "vkCreateDevice (+ VK_KHR_swapchain";
    if (pipeStatsSupported)
      devNote += ", pipelineStatisticsQuery";
    if (perfSetup.want)
      devNote += ", VK_KHR_performance_query";
    devNote += ")";
    steps.push_back(devNote);
  }

  VkQueue graphicsQueue = VK_NULL_HANDLE;
  VkQueue presentQueue = VK_NULL_HANDLE;
  vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
  vkGetDeviceQueue(device, presentFamily, 0, &presentQueue);

  auto vWords = SpirvToolchain::loadSpvFile(req.vertexSpv);
  auto fWords = SpirvToolchain::loadSpvFile(req.fragmentSpv);
  if (!vWords || !fWords) {
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return vWords ? fWords.error() : vWords.error();
  }
  steps.push_back("loaded vertex/fragment SPIR-V for WSI");

  auto vMod = createModule(device, vWords.value());
  auto fMod = createModule(device, fWords.value());
  if (!vMod || !fMod) {
    if (vMod)
      vkDestroyShaderModule(device, vMod.value(), nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return vMod ? fMod.error() : vMod.error();
  }

  // Sync X geometry before querying surface capabilities.
  XEvent mapEv;
  while (true) {
    XNextEvent(dpy, &mapEv);
    if (mapEv.type == MapNotify)
      break;
  }

  SwapchainSupport support = querySwapchain(physical, surface);
  if (support.formats.empty() || support.modes.empty()) {
    vkDestroyShaderModule(device, fMod.value(), nullptr);
    vkDestroyShaderModule(device, vMod.value(), nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return Error::make(ErrorCode::DriverError, "swapchain support query empty");
  }

  const VkSurfaceFormatKHR surfaceFormat = chooseFormat(support.formats);
  const VkPresentModeKHR presentMode = choosePresentMode(support.modes);
  const VkExtent2D extent =
      chooseExtent(support.caps, static_cast<uint32_t>(req.width),
                   static_cast<uint32_t>(req.height));

  uint32_t imageCount = support.caps.minImageCount + 1;
  if (support.caps.maxImageCount > 0 && imageCount > support.caps.maxImageCount)
    imageCount = support.caps.maxImageCount;

  VkSwapchainCreateInfoKHR sci{};
  sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  sci.surface = surface;
  sci.minImageCount = imageCount;
  sci.imageFormat = surfaceFormat.format;
  sci.imageColorSpace = surfaceFormat.colorSpace;
  sci.imageExtent = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  const uint32_t qIndices[] = {graphicsFamily, presentFamily};
  if (graphicsFamily != presentFamily) {
    sci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    sci.queueFamilyIndexCount = 2;
    sci.pQueueFamilyIndices = qIndices;
  } else {
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }
  sci.preTransform = support.caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = presentMode;
  sci.clipped = VK_TRUE;

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  r = vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain);
  if (r != VK_SUCCESS) {
    vkDestroyShaderModule(device, fMod.value(), nullptr);
    vkDestroyShaderModule(device, vMod.value(), nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return vkError("vkCreateSwapchainKHR failed", r);
  }
  steps.push_back("vkCreateSwapchainKHR format=" + std::to_string(surfaceFormat.format) +
                  " extent=" + std::to_string(extent.width) + "x" +
                  std::to_string(extent.height));

  uint32_t scImageCount = 0;
  vkGetSwapchainImagesKHR(device, swapchain, &scImageCount, nullptr);
  std::vector<VkImage> scImages(scImageCount);
  vkGetSwapchainImagesKHR(device, swapchain, &scImageCount, scImages.data());

  std::vector<VkImageView> scViews(scImageCount);
  for (uint32_t i = 0; i < scImageCount; ++i) {
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = scImages[i];
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = surfaceFormat.format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    r = vkCreateImageView(device, &vci, nullptr, &scViews[i]);
    if (r != VK_SUCCESS) {
      for (uint32_t j = 0; j < i; ++j)
        vkDestroyImageView(device, scViews[j], nullptr);
      vkDestroySwapchainKHR(device, swapchain, nullptr);
      vkDestroyShaderModule(device, fMod.value(), nullptr);
      vkDestroyShaderModule(device, vMod.value(), nullptr);
      vkDestroyDevice(device, nullptr);
      vkDestroySurfaceKHR(instance, surface, nullptr);
      vkDestroyInstance(instance, nullptr);
      XDestroyWindow(dpy, win);
      XCloseDisplay(dpy);
      return vkError("vkCreateImageView (swapchain) failed", r);
    }
  }

  VkAttachmentDescription colorAtt{};
  colorAtt.format = surfaceFormat.format;
  colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  colorAtt.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentReference colorRef{};
  colorRef.attachment = 0;
  colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;

  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = 1;
  rpci.pAttachments = &colorAtt;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &subpass;
  rpci.dependencyCount = 1;
  rpci.pDependencies = &dep;
  VkRenderPass renderPass = VK_NULL_HANDLE;
  r = vkCreateRenderPass(device, &rpci, nullptr, &renderPass);
  if (r != VK_SUCCESS) {
    for (auto v : scViews)
      vkDestroyImageView(device, v, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyShaderModule(device, fMod.value(), nullptr);
    vkDestroyShaderModule(device, vMod.value(), nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return vkError("vkCreateRenderPass failed", r);
  }

  std::vector<VkFramebuffer> framebuffers(scImageCount);
  for (uint32_t i = 0; i < scImageCount; ++i) {
    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = renderPass;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &scViews[i];
    fbci.width = extent.width;
    fbci.height = extent.height;
    fbci.layers = 1;
    r = vkCreateFramebuffer(device, &fbci, nullptr, &framebuffers[i]);
    if (r != VK_SUCCESS) {
      for (uint32_t j = 0; j < i; ++j)
        vkDestroyFramebuffer(device, framebuffers[j], nullptr);
      vkDestroyRenderPass(device, renderPass, nullptr);
      for (auto v : scViews)
        vkDestroyImageView(device, v, nullptr);
      vkDestroySwapchainKHR(device, swapchain, nullptr);
      vkDestroyShaderModule(device, fMod.value(), nullptr);
      vkDestroyShaderModule(device, vMod.value(), nullptr);
      vkDestroyDevice(device, nullptr);
      vkDestroySurfaceKHR(instance, surface, nullptr);
      vkDestroyInstance(instance, nullptr);
      XDestroyWindow(dpy, win);
      XCloseDisplay(dpy);
      return vkError("vkCreateFramebuffer failed", r);
    }
  }
  steps.push_back("swapchain framebuffers ready (" + std::to_string(scImageCount) + ")");

  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  vkCreatePipelineLayout(device, &plci, nullptr, &layout);

  VkPipelineShaderStageCreateInfo stagesInfo[2]{};
  stagesInfo[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stagesInfo[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stagesInfo[0].module = vMod.value();
  stagesInfo[0].pName = "main";
  stagesInfo[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stagesInfo[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stagesInfo[1].module = fMod.value();
  stagesInfo[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkViewport viewport{};
  viewport.width = static_cast<float>(extent.width);
  viewport.height = static_cast<float>(extent.height);
  viewport.maxDepth = 1.0f;
  VkRect2D scissor{};
  scissor.extent = extent;
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
  gpci.pStages = stagesInfo;
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
    vkDestroyPipelineLayout(device, layout, nullptr);
    for (auto fb : framebuffers)
      vkDestroyFramebuffer(device, fb, nullptr);
    vkDestroyRenderPass(device, renderPass, nullptr);
    for (auto v : scViews)
      vkDestroyImageView(device, v, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyShaderModule(device, fMod.value(), nullptr);
    vkDestroyShaderModule(device, vMod.value(), nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return vkError("vkCreateGraphicsPipelines failed", r);
  }
  steps.push_back("vkCreateGraphicsPipelines");

  VkCommandPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolCi.queueFamilyIndex = graphicsFamily;
  poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool cmdPool = VK_NULL_HANDLE;
  vkCreateCommandPool(device, &poolCi, nullptr, &cmdPool);

  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = cmdPool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(device, &cbai, &cmd);

  VkSemaphoreCreateInfo semCi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkFenceCreateInfo fenceCi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fenceCi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VkSemaphore imageAvailable = VK_NULL_HANDLE;
  VkSemaphore renderFinished = VK_NULL_HANDLE;
  VkFence inFlight = VK_NULL_HANDLE;
  vkCreateSemaphore(device, &semCi, nullptr, &imageAvailable);
  vkCreateSemaphore(device, &semCi, nullptr, &renderFinished);
  vkCreateFence(device, &fenceCi, nullptr, &inFlight);
  steps.push_back("sync: acquire/render semaphores + in-flight fence");

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
      steps.push_back("WSI pipeline statistics enabled");
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
      steps.push_back("WSI occlusion query enabled");
  }

  VulkanPerfQuerySession perf =
      vulkanCreatePerfQuerySession(instance, physical, device, graphicsFamily, steps,
                                   VendorCountersMode::PreferSinglePass);
  if (!perf.available) {
    perf.note = vulkanPerfQueryHint(meta);
    if (!steps.empty() && steps.back().rfind("vendor HW counters:", 0) == 0)
      steps.back() = "vendor HW counters: " + perf.note;
  }
  const bool wsiPerfOk = perf.available && perf.passCount == 1;
  if (perf.available && perf.passCount > 1)
    steps.push_back("WSI: multi-pass perf query skipped (passCount=" +
                    std::to_string(perf.passCount) + ")");

  const auto t0 = std::chrono::steady_clock::now();
  const auto deadline = t0 + std::chrono::milliseconds(std::max(req.presentMs, 1));
  uint32_t frames = 0;
  bool quit = false;
  std::vector<PipelineStats> frameCounters;

  while (!quit) {
    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);
      if (ev.type == KeyPress) {
        KeySym ks = XLookupKeysym(&ev.xkey, 0);
        if (ks == XK_Escape || ks == XK_q || ks == XK_Q)
          quit = true;
      } else if (ev.type == ClientMessage &&
                 static_cast<Atom>(ev.xclient.data.l[0]) == wmDelete) {
        quit = true;
      }
    }
    if (quit || std::chrono::steady_clock::now() >= deadline)
      break;

    vkWaitForFences(device, 1, &inFlight, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &inFlight);

    uint32_t imageIndex = 0;
    r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable, VK_NULL_HANDLE,
                              &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
      break;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
      break;

    const auto tRecord0 = std::chrono::steady_clock::now();
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    if (statsPool)
      vkCmdResetQueryPool(cmd, statsPool, 0, 1);
    if (occlusionPool)
      vkCmdResetQueryPool(cmd, occlusionPool, 0, 1);
    if (wsiPerfOk)
      vulkanPerfCmdReset(cmd, perf);
    if (statsPool)
      vkCmdBeginQuery(cmd, statsPool, 0, 0);
    if (occlusionPool)
      vkCmdBeginQuery(cmd, occlusionPool, 0, 0);
    if (wsiPerfOk)
      vulkanPerfCmdBegin(cmd, perf);

    VkClearValue clear{};
    clear.color = {{0.02f, 0.02f, 0.05f, 1.0f}};
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderPass;
    rpBegin.framebuffer = framebuffers[imageIndex];
    rpBegin.renderArea.extent = extent;
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    if (wsiPerfOk)
      vulkanPerfCmdEnd(cmd, perf);
    if (occlusionPool)
      vkCmdEndQuery(cmd, occlusionPool, 0);
    if (statsPool)
      vkCmdEndQuery(cmd, statsPool, 0);
    vkEndCommandBuffer(cmd);
    const auto tRecord1 = std::chrono::steady_clock::now();

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished;
    const auto tSubmit0 = std::chrono::steady_clock::now();
    r = vkQueueSubmit(graphicsQueue, 1, &submit, inFlight);
    if (r != VK_SUCCESS)
      break;
    const auto tSubmitEnd = std::chrono::steady_clock::now();

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    r = vkQueuePresentKHR(presentQueue, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
      ++frames;
      break;
    }
    if (r != VK_SUCCESS)
      break;
    ++frames;

    // Sample counters every frame (median later); wait fence for GPU completion.
    const auto tFence0 = std::chrono::steady_clock::now();
    vkWaitForFences(device, 1, &inFlight, VK_TRUE, UINT64_MAX);
    const auto tFence1 = std::chrono::steady_clock::now();
    PipelineStats frameStat;
    frameStat.hostRecordMs =
        std::chrono::duration<double, std::milli>(tRecord1 - tRecord0).count();
    frameStat.hostSubmitMs =
        std::chrono::duration<double, std::milli>(tSubmitEnd - tSubmit0).count();
    frameStat.hostQueueWaitMs =
        std::chrono::duration<double, std::milli>(tFence1 - tFence0).count();
    frameStat.drawCount = 1;
    if (statsPool) {
      std::vector<uint64_t> raw(statCount, 0);
      if (vulkanGetQueryResultsU64(device, statsPool, 0, 1, raw.data(), sizeof(uint64_t))) {
        frameStat = parsePipelineStats(statFlags, raw.data(), statCount);
        frameStat.hostRecordMs =
            std::chrono::duration<double, std::milli>(tRecord1 - tRecord0).count();
        frameStat.hostSubmitMs =
            std::chrono::duration<double, std::milli>(tSubmitEnd - tSubmit0).count();
        frameStat.hostQueueWaitMs =
            std::chrono::duration<double, std::milli>(tFence1 - tFence0).count();
        frameStat.drawCount = 1;
      }
    }
    if (wsiPerfOk) {
      std::string vNote;
      if (vulkanPerfQueryResults(device, perf, frameStat.vendor, vNote)) {
        frameStat.vendorApi = "VK_KHR_performance_query";
        frameStat.vendorNote = vNote;
      }
    } else if (perf.available && perf.passCount > 1) {
      frameStat.vendorNote = "multi-pass perf query skipped on WSI";
    }
    if (occlusionPool) {
      uint64_t occ = 0;
      if (vulkanGetQueryResultsU64(device, occlusionPool, 0, 1, &occ, sizeof(uint64_t))) {
        frameStat.occlusionAvailable = true;
        frameStat.occlusionSamplesPassed = occ;
      }
    }
    frameCounters.push_back(frameStat);
  }

  vkDeviceWaitIdle(device);
  const auto t1 = std::chrono::steady_clock::now();
  out.hostMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  out.framesPresented = frames;
  out.ok = frames > 0;
  if (!frameCounters.empty())
    out.counters = PipelineStats::medianOf(frameCounters);
  steps.push_back("presented " + std::to_string(frames) + " frames via vkQueuePresentKHR");
  if (out.counters.available || out.counters.occlusionAvailable)
    steps.push_back(out.counters.summaryLine() + " (median over frames)");

  vulkanDestroyPerfQuerySession(device, perf);
  if (occlusionPool)
    vkDestroyQueryPool(device, occlusionPool, nullptr);
  if (statsPool)
    vkDestroyQueryPool(device, statsPool, nullptr);
  vkDestroyFence(device, inFlight, nullptr);
  vkDestroySemaphore(device, renderFinished, nullptr);
  vkDestroySemaphore(device, imageAvailable, nullptr);
  vkDestroyCommandPool(device, cmdPool, nullptr);
  vkDestroyPipeline(device, pipeline, nullptr);
  vkDestroyPipelineLayout(device, layout, nullptr);
  for (auto fb : framebuffers)
    vkDestroyFramebuffer(device, fb, nullptr);
  vkDestroyRenderPass(device, renderPass, nullptr);
  for (auto v : scViews)
    vkDestroyImageView(device, v, nullptr);
  vkDestroySwapchainKHR(device, swapchain, nullptr);
  vkDestroyShaderModule(device, fMod.value(), nullptr);
  vkDestroyShaderModule(device, vMod.value(), nullptr);
  vkDestroyDevice(device, nullptr);
  vkDestroySurfaceKHR(instance, surface, nullptr);
  vkDestroyInstance(instance, nullptr);
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);
  steps.push_back("WSI cleanup complete");

  logInfo("runtime", "Vulkan WSI present OK (" + std::to_string(frames) + " frames) on " +
                         out.deviceName);
  return out;
}

} // namespace csrun
