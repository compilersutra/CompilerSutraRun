#include "csrun/backends/hip/HipBackend.h"

#include "csrun/benchmark/PowerSampler.h"
#include "csrun/core/Log.h"
#include "csrun/workload/VectorAddScaffold.h"

#include <hip/hip_runtime.h>
#include <hip/hiprtc.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace csrun {
namespace {

Expected<std::string> readFile(const std::filesystem::path &path) {
  std::ifstream in(path);
  if (!in)
    return Error::make(ErrorCode::IoError, "failed to read " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string hipErr(hipError_t e) {
  const char *s = hipGetErrorString(e);
  return std::string("HIP: ") + (s ? s : "unknown error");
}

Expected<void> checkHip(hipError_t e, const char *what) {
  if (e == hipSuccess)
    return {};
  return Error::make(ErrorCode::DriverError, std::string(what) + ": " + hipErr(e));
}

} // namespace

BackendInfo HipBackend::info() const {
  BackendInfo i;
  i.type = BackendType::HIP;
  i.name = "hip";
  auto devices = enumerateDevices();
  if (devices && !devices.value().empty()) {
    i.available = true;
    i.availabilityNote =
        std::to_string(devices.value().size()) + " device(s): " + devices.value().front().name;
  } else {
    i.available = false;
    i.availabilityNote = devices ? "no HIP device" : devices.error().message;
  }
  return i;
}

Expected<std::vector<DeviceInfo>> HipBackend::enumerateDevices() const {
  int count = 0;
  hipError_t e = hipGetDeviceCount(&count);
  if (e != hipSuccess || count <= 0)
    return Error::make(ErrorCode::DeviceNotFound, "no HIP devices (" + hipErr(e) + ")");

  std::vector<DeviceInfo> out;
  out.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    hipDeviceProp_t prop{};
    e = hipGetDeviceProperties(&prop, i);
    if (e != hipSuccess)
      continue;
    DeviceInfo d;
    d.backend = BackendType::HIP;
    d.index = static_cast<uint32_t>(i);
    d.name = prop.name;
    d.vendor = "AMD";
    d.driver = prop.gcnArchName;
    d.available = true;
    out.push_back(std::move(d));
  }
  if (out.empty())
    return Error::make(ErrorCode::DeviceNotFound, "no usable HIP devices");
  return out;
}

Expected<RunResult> HipBackend::execute(const Workload &workload, const DeviceInfo &device,
                                        const ExecuteOptions &options) const {
  auto nOr = requireVectorAddFp32(workload, "hip");
  if (!nOr)
    return nOr.error();
  const uint32_t n = nOr.value();

  static constexpr std::string_view kFallbacks[] = {
      "examples/vector_add/vector_add.hip",
      "../examples/vector_add/vector_add.hip",
      "../../examples/vector_add/vector_add.hip",
  };
  auto kernelPath = resolveKernelPath(workload, "vector_add.hip", kFallbacks);
  if (!kernelPath)
    return kernelPath.error();
  auto src = readFile(kernelPath.value());
  if (!src)
    return src.error();

  const int deviceId = static_cast<int>(device.index);
  if (auto set = checkHip(hipSetDevice(deviceId), "hipSetDevice"); !set)
    return set.error();

  hipDeviceProp_t prop{};
  if (auto gp = checkHip(hipGetDeviceProperties(&prop, deviceId), "hipGetDeviceProperties"); !gp)
    return gp.error();

  hiprtcProgram prog = nullptr;
  hiprtcResult rc =
      hiprtcCreateProgram(&prog, src.value().c_str(), "vector_add.hip", 0, nullptr, nullptr);
  if (rc != HIPRTC_SUCCESS || !prog) {
    return Error::make(ErrorCode::CompilationFailed,
                       std::string("hiprtcCreateProgram: ") + hiprtcGetErrorString(rc));
  }

  const std::string archOpt = std::string("--gpu-architecture=") + prop.gcnArchName;
  const char *opts[] = {archOpt.c_str()};
  rc = hiprtcCompileProgram(prog, 1, opts);
  if (rc != HIPRTC_SUCCESS) {
    size_t logSize = 0;
    hiprtcGetProgramLogSize(prog, &logSize);
    std::string log(logSize, '\0');
    if (logSize)
      hiprtcGetProgramLog(prog, log.data());
    hiprtcDestroyProgram(&prog);
    Error e = Error::make(ErrorCode::CompilationFailed, "hiprtcCompileProgram failed: " + log);
    e.backend = "hip";
    return e;
  }

  size_t codeSize = 0;
  hiprtcGetCodeSize(prog, &codeSize);
  std::vector<char> code(codeSize);
  hiprtcGetCode(prog, code.data());
  hiprtcDestroyProgram(&prog);

  hipModule_t module = nullptr;
  if (auto ld = checkHip(hipModuleLoadData(&module, code.data()), "hipModuleLoadData"); !ld)
    return ld.error();

  hipFunction_t fn = nullptr;
  if (auto gf = checkHip(hipModuleGetFunction(&fn, module, "vector_add"), "hipModuleGetFunction");
      !gf) {
    hipModuleUnload(module);
    return gf.error();
  }

  VectorAddHost host = makeVectorAddHost(n);
  const size_t bytes = static_cast<size_t>(n) * sizeof(float);
  float *devA = nullptr;
  float *devB = nullptr;
  float *devC = nullptr;
  auto failClean = [&](Error err) -> Expected<RunResult> {
    if (devA)
      hipFree(devA);
    if (devB)
      hipFree(devB);
    if (devC)
      hipFree(devC);
    if (module)
      hipModuleUnload(module);
    return err;
  };

  if (auto a = checkHip(hipMalloc(&devA, bytes), "hipMalloc A"); !a)
    return failClean(a.error());
  if (auto b = checkHip(hipMalloc(&devB, bytes), "hipMalloc B"); !b)
    return failClean(b.error());
  if (auto c = checkHip(hipMalloc(&devC, bytes), "hipMalloc C"); !c)
    return failClean(c.error());

  if (auto cpy = checkHip(hipMemcpy(devA, host.a.data(), bytes, hipMemcpyHostToDevice), "H2D A");
      !cpy)
    return failClean(cpy.error());
  if (auto cpy = checkHip(hipMemcpy(devB, host.b.data(), bytes, hipMemcpyHostToDevice), "H2D B");
      !cpy)
    return failClean(cpy.error());

  unsigned int count = n;
  void *args[] = {&devA, &devB, &devC, &count};
  const unsigned int block = 256;
  const unsigned int grid = (n + block - 1) / block;
  const int warmupIters = options.benchmark ? workload.execution.warmup : 0;
  const int timedIters = options.benchmark ? workload.execution.iterations : 1;

  PowerSampler power;

  hipEvent_t start = nullptr;
  hipEvent_t stop = nullptr;
  const bool haveEvents =
      hipEventCreate(&start) == hipSuccess && hipEventCreate(&stop) == hipSuccess;

  auto dispatchOnce = [&](bool timed) -> Expected<double> {
    if (timed && haveEvents)
      hipEventRecord(start, nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    hipError_t e =
        hipModuleLaunchKernel(fn, grid, 1, 1, block, 1, 1, 0, nullptr, args, nullptr);
    if (e != hipSuccess)
      return Error::make(ErrorCode::ExecutionFailed, "hipModuleLaunchKernel: " + hipErr(e));
    e = hipDeviceSynchronize();
    if (e != hipSuccess)
      return Error::make(ErrorCode::ExecutionFailed, "hipDeviceSynchronize: " + hipErr(e));
    const auto t1 = std::chrono::steady_clock::now();
    if (timed && haveEvents) {
      hipEventRecord(stop, nullptr);
      hipEventSynchronize(stop);
      float ms = 0.f;
      hipEventElapsedTime(&ms, start, stop);
      return static_cast<double>(ms);
    }
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
  };

  for (int i = 0; i < warmupIters; ++i) {
    auto w = dispatchOnce(false);
    if (!w) {
      if (haveEvents) {
        hipEventDestroy(start);
        hipEventDestroy(stop);
      }
      return failClean(w.error());
    }
  }

  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(timedIters));
  TimingSource timingSource = haveEvents ? TimingSource::GpuTimestamp : TimingSource::CpuWall;
  for (int i = 0; i < timedIters; ++i) {
    auto ms = dispatchOnce(true);
    if (!ms) {
      if (haveEvents) {
        hipEventDestroy(start);
        hipEventDestroy(stop);
      }
      return failClean(ms.error());
    }
    samples.push_back(ms.value());
  }

  if (auto cpy = checkHip(hipMemcpy(host.output.data(), devC, bytes, hipMemcpyDeviceToHost), "D2H");
      !cpy) {
    if (haveEvents) {
      hipEventDestroy(start);
      hipEventDestroy(stop);
    }
    return failClean(cpy.error());
  }

  DeviceInfo report = device;
  if (report.name.empty())
    report.name = prop.name;
  if (report.vendor.empty())
    report.vendor = "AMD";
  if (report.driver.empty())
    report.driver = prop.gcnArchName;

  RunResult result = fillVectorAddResult(workload, report, BackendType::HIP, "hiprtc", options,
                                         samples, timingSource, host.reference, host.output);
  result.power = power.stats();

  if (haveEvents) {
    hipEventDestroy(start);
    hipEventDestroy(stop);
  }
  hipFree(devA);
  hipFree(devB);
  hipFree(devC);
  hipModuleUnload(module);

  logInfo("runtime", "HIP vector_add completed on " + result.deviceName);
  return result;
}

std::unique_ptr<Backend> createHipBackend() {
  return std::make_unique<HipBackend>();
}

} // namespace csrun
