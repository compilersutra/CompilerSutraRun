#include "csrun/backends/hip/HipBackend.h"

#include "csrun/benchmark/PowerSampler.h"
#include "csrun/core/Log.h"
#include "csrun/workload/MatmulScaffold.h"
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
  const bool isMatmul = workload.operation == "matmul";
  MatmulDims matDims{};
  uint32_t n = 0;
  if (isMatmul) {
    auto dOr = requireMatmulFp32(workload, "hip");
    if (!dOr)
      return dOr.error();
    matDims = dOr.value();
  } else {
    auto nOr = requireVectorAddFp32(workload, "hip");
    if (!nOr)
      return nOr.error();
    n = nOr.value();
  }

  static constexpr std::string_view kVecFallbacks[] = {
      "examples/vector_add/vector_add.hip",
      "../examples/vector_add/vector_add.hip",
      "../../examples/vector_add/vector_add.hip",
  };
  static constexpr std::string_view kMatFallbacks[] = {
      "examples/matmul/matmul.hip",
      "../examples/matmul/matmul.hip",
      "../../examples/matmul/matmul.hip",
  };
  auto kernelPath = isMatmul ? resolveKernelPath(workload, "matmul.hip", kMatFallbacks)
                             : resolveKernelPath(workload, "vector_add.hip", kVecFallbacks);
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
  const char *srcName = isMatmul ? "matmul.hip" : "vector_add.hip";
  hiprtcResult rc =
      hiprtcCreateProgram(&prog, src.value().c_str(), srcName, 0, nullptr, nullptr);
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
  const char *fnName = isMatmul ? "matmul" : "vector_add";
  if (auto gf = checkHip(hipModuleGetFunction(&fn, module, fnName), "hipModuleGetFunction"); !gf) {
    hipModuleUnload(module);
    return gf.error();
  }

  VectorAddHost vecHost;
  MatmulHost matHost;
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

  unsigned int count = n;
  unsigned int M = matDims.M, N = matDims.N, K = matDims.K;
  unsigned int gridX = 1, gridY = 1, blockX = 256, blockY = 1;
  void *argsVec[] = {&devA, &devB, &devC, &count};
  void *argsMat[] = {&devA, &devB, &devC, &M, &N, &K};
  void **args = isMatmul ? argsMat : argsVec;

  if (isMatmul) {
    matHost = makeMatmulHost(matDims);
    const size_t aBytes = static_cast<size_t>(M) * K * sizeof(float);
    const size_t bBytes = static_cast<size_t>(K) * N * sizeof(float);
    const size_t cBytes = static_cast<size_t>(M) * N * sizeof(float);
    if (auto a = checkHip(hipMalloc(&devA, aBytes), "hipMalloc A"); !a)
      return failClean(a.error());
    if (auto b = checkHip(hipMalloc(&devB, bBytes), "hipMalloc B"); !b)
      return failClean(b.error());
    if (auto c = checkHip(hipMalloc(&devC, cBytes), "hipMalloc C"); !c)
      return failClean(c.error());
    if (auto cpy = checkHip(hipMemcpy(devA, matHost.a.data(), aBytes, hipMemcpyHostToDevice), "H2D A");
        !cpy)
      return failClean(cpy.error());
    if (auto cpy = checkHip(hipMemcpy(devB, matHost.b.data(), bBytes, hipMemcpyHostToDevice), "H2D B");
        !cpy)
      return failClean(cpy.error());
    blockX = 16;
    blockY = 16;
    gridX = (N + blockX - 1) / blockX;
    gridY = (M + blockY - 1) / blockY;
  } else {
    vecHost = makeVectorAddHost(n);
    const size_t bytes = static_cast<size_t>(n) * sizeof(float);
    if (auto a = checkHip(hipMalloc(&devA, bytes), "hipMalloc A"); !a)
      return failClean(a.error());
    if (auto b = checkHip(hipMalloc(&devB, bytes), "hipMalloc B"); !b)
      return failClean(b.error());
    if (auto c = checkHip(hipMalloc(&devC, bytes), "hipMalloc C"); !c)
      return failClean(c.error());
    if (auto cpy = checkHip(hipMemcpy(devA, vecHost.a.data(), bytes, hipMemcpyHostToDevice), "H2D A");
        !cpy)
      return failClean(cpy.error());
    if (auto cpy = checkHip(hipMemcpy(devB, vecHost.b.data(), bytes, hipMemcpyHostToDevice), "H2D B");
        !cpy)
      return failClean(cpy.error());
    gridX = (n + blockX - 1) / blockX;
  }

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
    hipError_t e = hipModuleLaunchKernel(fn, gridX, gridY, 1, blockX, blockY, 1, 0, nullptr, args,
                                         nullptr);
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

  const size_t outBytes = isMatmul ? static_cast<size_t>(M) * N * sizeof(float)
                                   : static_cast<size_t>(n) * sizeof(float);
  float *hostOut = isMatmul ? matHost.output.data() : vecHost.output.data();
  if (auto cpy = checkHip(hipMemcpy(hostOut, devC, outBytes, hipMemcpyDeviceToHost), "D2H"); !cpy) {
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

  RunResult result =
      isMatmul ? fillMatmulResult(workload, report, BackendType::HIP, "hiprtc", options, samples,
                                  timingSource, matHost.reference, matHost.output)
               : fillVectorAddResult(workload, report, BackendType::HIP, "hiprtc", options, samples,
                                     timingSource, vecHost.reference, vecHost.output);
  result.power = power.stats();

  if (haveEvents) {
    hipEventDestroy(start);
    hipEventDestroy(stop);
  }
  hipFree(devA);
  hipFree(devB);
  hipFree(devC);
  hipModuleUnload(module);

  logInfo("runtime", std::string("HIP ") + (isMatmul ? "matmul" : "vector_add") +
                         " completed on " + result.deviceName);
  return result;
}

std::unique_ptr<Backend> createHipBackend() {
  return std::make_unique<HipBackend>();
}

} // namespace csrun
