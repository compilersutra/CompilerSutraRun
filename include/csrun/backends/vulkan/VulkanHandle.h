//===----------------------------------------------------------------------===//
// VulkanHandle.h
// Move-only owning wrappers for Vulkan handles.
//===----------------------------------------------------------------------===//
#ifndef CSRUN_BACKENDS_VULKAN_VULKANHANDLE_H
#define CSRUN_BACKENDS_VULKAN_VULKANHANDLE_H

#include <vulkan/vulkan.h>

#include <utility>

namespace csrun {

/// Move-only owning handle.
///
/// Runs `D(handle)` exactly once when the last owner goes out of scope -- including
/// on an early `return` or a thrown exception, which is the point: a Vulkan backend
/// has many exit points and every one of them used to need a hand-written teardown.
///
/// A default-constructed handle owns nothing, so destruction is always safe.
template <typename T, typename D> class Unique {
public:
  Unique() = default;

  explicit Unique(T handle) : handle_(handle) {}

  /// Takes ownership with a deleter that may capture context (e.g. the parent `VkDevice`).
  Unique(T handle, D deleter) : handle_(handle), deleter_(std::move(deleter)) {}

  ~Unique() { reset(); }

  Unique(const Unique &) = delete;
  Unique &operator=(const Unique &) = delete;

  Unique(Unique &&other) noexcept
      : handle_(other.release()), deleter_(std::move(other.deleter_)) {}

  Unique &operator=(Unique &&other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.release();
      deleter_ = std::move(other.deleter_);
    }
    return *this;
  }

  /// The raw handle. Still owned by this object; do not destroy it yourself.
  T get() const { return handle_; }

  explicit operator bool() const { return handle_ != T{}; }

  /// Give up ownership without destroying. The caller becomes responsible.
  T release() noexcept {
    T out = handle_;
    handle_ = T{};
    return out;
  }

  /// Destroy now (if owned) and become empty. Safe to call more than once.
  void reset() {
    if (handle_ != T{}) {
      deleter_(handle_);
      handle_ = T{};
    }
  }

private:
  T handle_{};
  D deleter_{};
};

/// Deleter for a `vkDestroy*` entry point that takes a parent object first.
///
/// `Parent` is `VkDevice` or `VkInstance`; it is captured by value at construction
/// so the handle stays self-sufficient even if the owning scope is torn down first.
template <typename H, typename Parent> struct DestroyFn {
  using Fn = void (*)(Parent, H, const VkAllocationCallbacks *);
  Parent parent{};
  Fn fn = nullptr;

  void operator()(H h) const {
    if (h != H{} && fn != nullptr)
      fn(parent, h, nullptr);
  }
};

/// `vkDestroyInstance` / `vkDestroyDevice` take no parent object, so they get their
/// own deleters rather than the parent-first `DestroyFn` below.
struct DestroyInstance {
  void operator()(VkInstance instance) const {
    if (instance != VK_NULL_HANDLE)
      vkDestroyInstance(instance, nullptr);
  }
};

struct DestroyDevice {
  void operator()(VkDevice device) const {
    if (device != VK_NULL_HANDLE)
      vkDestroyDevice(device, nullptr);
  }
};

using VkInstanceHandle = Unique<VkInstance, DestroyInstance>;
using VkDeviceHandle = Unique<VkDevice, DestroyDevice>;

/// Types whose destroy entry point takes `(VkDevice, T, const VkAllocationCallbacks*)`.
#define CSRUN_VK_DEVICE_HANDLE(Type)                                                                \
  using Type##Handle = Unique<Type, DestroyFn<Type, VkDevice>>;

/// Types whose destroy entry point takes `(VkInstance, T, const VkAllocationCallbacks*)`.
#define CSRUN_VK_INSTANCE_HANDLE(Type)                                                              \
  using Type##Handle = Unique<Type, DestroyFn<Type, VkInstance>>;

CSRUN_VK_DEVICE_HANDLE(VkBuffer)
CSRUN_VK_DEVICE_HANDLE(VkDeviceMemory)
CSRUN_VK_DEVICE_HANDLE(VkShaderModule)
CSRUN_VK_DEVICE_HANDLE(VkPipeline)
CSRUN_VK_DEVICE_HANDLE(VkPipelineLayout)
CSRUN_VK_DEVICE_HANDLE(VkDescriptorSetLayout)
CSRUN_VK_DEVICE_HANDLE(VkDescriptorPool)
CSRUN_VK_DEVICE_HANDLE(VkCommandPool)
CSRUN_VK_DEVICE_HANDLE(VkQueryPool)
CSRUN_VK_DEVICE_HANDLE(VkCommandBuffer)
CSRUN_VK_DEVICE_HANDLE(VkSemaphore)
CSRUN_VK_DEVICE_HANDLE(VkFence)
CSRUN_VK_DEVICE_HANDLE(VkImage)
CSRUN_VK_DEVICE_HANDLE(VkImageView)
CSRUN_VK_DEVICE_HANDLE(VkSampler)
CSRUN_VK_DEVICE_HANDLE(VkFramebuffer)
CSRUN_VK_DEVICE_HANDLE(VkRenderPass)
CSRUN_VK_DEVICE_HANDLE(VkPipelineCache)
CSRUN_VK_DEVICE_HANDLE(VkDescriptorUpdateTemplate)

#undef CSRUN_VK_DEVICE_HANDLE
#undef CSRUN_VK_INSTANCE_HANDLE

} // namespace csrun

#endif // CSRUN_BACKENDS_VULKAN_VULKANHANDLE_H
