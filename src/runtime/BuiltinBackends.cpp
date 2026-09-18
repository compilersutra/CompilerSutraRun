#include "csrun/runtime/BuiltinBackends.h"

#include "csrun/core/Log.h"

#if defined(CSRUN_HAS_VULKAN)
#include "csrun/backends/vulkan/VulkanBackend.h"
#endif
#if defined(CSRUN_HAS_OPENGL)
#include "csrun/backends/opengl/OpenGLBackend.h"
#endif
#if defined(CSRUN_HAS_OPENCL)
#include "csrun/backends/opencl/OpenCLBackend.h"
#endif
#if defined(CSRUN_HAS_HIP)
#include "csrun/backends/hip/HipBackend.h"
#endif

namespace csrun {
namespace {

class StubBackend final : public Backend {
public:
  explicit StubBackend(BackendType type, std::string note)
      : type_(type), note_(std::move(note)) {}

  BackendInfo info() const override {
    BackendInfo i;
    i.type = type_;
    i.name = std::string(toString(type_));
    i.available = false;
    i.availabilityNote = note_;
    return i;
  }

  Expected<std::vector<DeviceInfo>> enumerateDevices() const override {
    return Error::make(ErrorCode::BackendUnavailable,
                       std::string(toString(type_)) + " backend unavailable: " + note_);
  }

  Expected<RunResult> execute(const Workload &, const DeviceInfo &,
                              const ExecuteOptions &) const override {
    return Error::make(ErrorCode::BackendUnavailable,
                       std::string(toString(type_)) + " backend unavailable: " + note_);
  }

private:
  BackendType type_;
  std::string note_;
};

void registerStub(BackendRegistry &reg, BackendType type, std::string note) {
  reg.registerBackend(std::make_unique<StubBackend>(type, std::move(note)));
}

} // namespace

BackendRegistry createDefaultBackendRegistry() {
  BackendRegistry reg;

#if defined(CSRUN_HAS_VULKAN)
  reg.registerBackend(createVulkanBackend());
#else
  registerStub(reg, BackendType::Vulkan,
               "Vulkan backend not built (configure with -DCSRUN_ENABLE_VULKAN=ON)");
#endif

#if defined(CSRUN_HAS_OPENGL)
  reg.registerBackend(createOpenGLBackend());
#else
  registerStub(reg, BackendType::OpenGL,
               "OpenGL backend not built (configure with -DCSRUN_ENABLE_OPENGL=ON)");
#endif

  registerStub(reg, BackendType::OpenGLES,
               "OpenGL ES backend not built yet (mobile/embedded GL ES path)");

#if defined(CSRUN_HAS_OPENCL)
  reg.registerBackend(createOpenCLBackend());
#else
  registerStub(reg, BackendType::OpenCL,
               "OpenCL backend not built (configure with -DCSRUN_ENABLE_OPENCL=ON)");
#endif

  registerStub(reg, BackendType::CUDA, "CUDA backend not built yet (PTX / NVIDIA)");

#if defined(CSRUN_HAS_HIP)
  reg.registerBackend(createHipBackend());
#else
  registerStub(reg, BackendType::HIP,
               "HIP/ROCm backend not built (configure with -DCSRUN_ENABLE_HIP=ON)");
#endif

  registerStub(reg, BackendType::D3D12, "D3D12/DXIL backend not built yet (Windows)");
  registerStub(reg, BackendType::Metal, "Metal backend not built yet (Apple)");
  registerStub(reg, BackendType::WebGPU, "WebGPU backend not built yet");
  registerStub(reg, BackendType::CPU, "CPU backend not built yet (host reference path)");

  logInfo("backend", "default registry created");
  return reg;
}

} // namespace csrun
