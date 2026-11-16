#include "csrun/backends/opencl/OpenCLBackend.h"

#include "csrun/benchmark/PowerSampler.h"
#include "csrun/core/Log.h"
#include "csrun/workload/MatmulScaffold.h"
#include "csrun/workload/VectorAddScaffold.h"

#define CL_TARGET_OPENCL_VERSION 300
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#include <CL/cl.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
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

std::string clErr(cl_int err) { return "OpenCL error " + std::to_string(err); }

struct ClDevice {
  cl_platform_id platform = nullptr;
  cl_device_id device = nullptr;
  std::string name;
  std::string vendor;
  std::string driver;
  uint32_t index = 0;
};

Expected<std::vector<ClDevice>> listClDevices() {
  cl_uint nPlatforms = 0;
  cl_int err = clGetPlatformIDs(0, nullptr, &nPlatforms);
  if (err != CL_SUCCESS || nPlatforms == 0)
    return Error::make(ErrorCode::DeviceNotFound, "no OpenCL platforms");

  std::vector<cl_platform_id> platforms(nPlatforms);
  err = clGetPlatformIDs(nPlatforms, platforms.data(), nullptr);
  if (err != CL_SUCCESS)
    return Error::make(ErrorCode::DriverError, clErr(err));

  std::vector<ClDevice> out;
  uint32_t index = 0;
  for (cl_platform_id plat : platforms) {
    cl_uint nDev = 0;
    err = clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 0, nullptr, &nDev);
    if (err != CL_SUCCESS || nDev == 0)
      continue;
    std::vector<cl_device_id> devices(nDev);
    err = clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, nDev, devices.data(), nullptr);
    if (err != CL_SUCCESS)
      continue;
    for (cl_device_id dev : devices) {
      char name[256] = {};
      char vendor[256] = {};
      char driver[256] = {};
      clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(name), name, nullptr);
      clGetDeviceInfo(dev, CL_DEVICE_VENDOR, sizeof(vendor), vendor, nullptr);
      clGetDeviceInfo(dev, CL_DRIVER_VERSION, sizeof(driver), driver, nullptr);
      ClDevice d;
      d.platform = plat;
      d.device = dev;
      d.name = name;
      d.vendor = vendor;
      d.driver = driver;
      d.index = index++;
      out.push_back(std::move(d));
    }
  }
  if (out.empty())
    return Error::make(ErrorCode::DeviceNotFound, "no OpenCL GPU devices");
  return out;
}

Expected<ClDevice> pickDevice(const DeviceInfo &want) {
  auto all = listClDevices();
  if (!all)
    return all.error();
  // Prefer exact physical index (set by --device-index / PlatformRuntime).
  for (const auto &d : all.value()) {
    if (d.index == want.index)
      return d;
  }
  for (const auto &d : all.value()) {
    if (!want.name.empty() && d.name == want.name)
      return d;
  }
  return Error::make(ErrorCode::DeviceNotFound,
                     "OpenCL device index " + std::to_string(want.index) +
                         " not found (see --list-devices)");
}

} // namespace

BackendInfo OpenCLBackend::info() const {
  BackendInfo i;
  i.type = BackendType::OpenCL;
  i.name = "opencl";
  auto devices = enumerateDevices();
  if (devices && !devices.value().empty()) {
    i.available = true;
    i.availabilityNote =
        std::to_string(devices.value().size()) + " device(s): " + devices.value().front().name;
  } else {
    i.available = false;
    i.availabilityNote = devices ? "no OpenCL GPU" : devices.error().message;
  }
  return i;
}

Expected<std::vector<DeviceInfo>> OpenCLBackend::enumerateDevices() const {
  auto clDevs = listClDevices();
  if (!clDevs)
    return clDevs.error();
  std::vector<DeviceInfo> out;
  out.reserve(clDevs.value().size());
  for (const auto &d : clDevs.value()) {
    DeviceInfo info;
    info.backend = BackendType::OpenCL;
    info.index = d.index;
    info.name = d.name;
    info.vendor = d.vendor;
    info.driver = d.driver;
    info.available = true;
    out.push_back(std::move(info));
  }
  return out;
}

namespace {

Expected<std::pair<cl_command_queue, bool>> makeClQueue(cl_context ctx, cl_device_id device) {
  cl_int err = CL_SUCCESS;
  bool profilingQueue = true;
  cl_queue_properties qprops[] = {CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0};
  cl_command_queue queue = clCreateCommandQueueWithProperties(ctx, device, qprops, &err);
  if (err != CL_SUCCESS || !queue) {
    profilingQueue = false;
    queue = clCreateCommandQueueWithProperties(ctx, device, nullptr, &err);
  }
  if (err != CL_SUCCESS || !queue) {
    profilingQueue = false;
    queue = clCreateCommandQueue(ctx, device, CL_QUEUE_PROFILING_ENABLE, &err);
    if (err == CL_SUCCESS && queue)
      profilingQueue = true;
  }
  if (err != CL_SUCCESS || !queue)
    queue = clCreateCommandQueue(ctx, device, 0, &err);
  if (err != CL_SUCCESS || !queue)
    return Error::make(ErrorCode::DriverError, "clCreateCommandQueue: " + clErr(err));
  return std::make_pair(queue, profilingQueue);
}

Expected<double> profileOrWall(cl_command_queue queue, cl_kernel kernel, cl_uint workDim,
                               const size_t *global, const size_t *local, bool timed,
                               bool profilingQueue, TimingSource &timingSource) {
  cl_event ev = nullptr;
  cl_event *evPtr = (timed && profilingQueue) ? &ev : nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  cl_int e = clEnqueueNDRangeKernel(queue, kernel, workDim, nullptr, global, local, 0, nullptr,
                                    evPtr);
  if (e != CL_SUCCESS)
    return Error::make(ErrorCode::ExecutionFailed, "clEnqueueNDRangeKernel: " + clErr(e));
  if (ev) {
    e = clWaitForEvents(1, &ev);
    if (e != CL_SUCCESS) {
      clReleaseEvent(ev);
      return Error::make(ErrorCode::ExecutionFailed, "clWaitForEvents: " + clErr(e));
    }
    cl_ulong startNs = 0;
    cl_ulong endNs = 0;
    const cl_int e0 =
        clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_START, sizeof(startNs), &startNs, nullptr);
    const cl_int e1 =
        clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_END, sizeof(endNs), &endNs, nullptr);
    clReleaseEvent(ev);
    if (e0 == CL_SUCCESS && e1 == CL_SUCCESS && endNs >= startNs)
      return static_cast<double>(endNs - startNs) / 1.0e6;
    timingSource = TimingSource::CpuWall;
  }
  e = clFinish(queue);
  if (e != CL_SUCCESS)
    return Error::make(ErrorCode::ExecutionFailed, "clFinish: " + clErr(e));
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

} // namespace

Expected<RunResult> OpenCLBackend::execute(const Workload &workload, const DeviceInfo &device,
                                           const ExecuteOptions &options) const {
  const bool isMatmul = workload.operation == "matmul";
  if (!isMatmul) {
    auto nOr = requireVectorAddFp32(workload, "opencl");
    if (!nOr)
      return nOr.error();
  } else {
    auto dOr = requireMatmulFp32(workload, "opencl");
    if (!dOr)
      return dOr.error();
  }

  static constexpr std::string_view kVecFallbacks[] = {
      "examples/vector_add/vector_add.cl",
      "../examples/vector_add/vector_add.cl",
      "../../examples/vector_add/vector_add.cl",
  };
  static constexpr std::string_view kMatFallbacks[] = {
      "examples/matmul/matmul.cl",
      "../examples/matmul/matmul.cl",
      "../../examples/matmul/matmul.cl",
  };
  auto kernelPath =
      isMatmul ? resolveKernelPath(workload, "matmul.cl", kMatFallbacks)
               : resolveKernelPath(workload, "vector_add.cl", kVecFallbacks);
  if (!kernelPath)
    return kernelPath.error();
  auto src = readFile(kernelPath.value());
  if (!src)
    return src.error();

  auto picked = pickDevice(device);
  if (!picked)
    return picked.error();
  const ClDevice &dev = picked.value();

  cl_int err = CL_SUCCESS;
  cl_context_properties props[] = {CL_CONTEXT_PLATFORM,
                                   reinterpret_cast<cl_context_properties>(dev.platform), 0};
  cl_context ctx = clCreateContext(props, 1, &dev.device, nullptr, nullptr, &err);
  if (err != CL_SUCCESS || !ctx)
    return Error::make(ErrorCode::DriverError, "clCreateContext: " + clErr(err));

  auto queueOr = makeClQueue(ctx, dev.device);
  if (!queueOr) {
    clReleaseContext(ctx);
    return queueOr.error();
  }
  cl_command_queue queue = queueOr.value().first;
  bool profilingQueue = queueOr.value().second;

  const char *srcPtr = src.value().c_str();
  size_t srcLen = src.value().size();
  cl_program program = clCreateProgramWithSource(ctx, 1, &srcPtr, &srcLen, &err);
  if (err != CL_SUCCESS || !program) {
    clReleaseCommandQueue(queue);
    clReleaseContext(ctx);
    return Error::make(ErrorCode::CompilationFailed, "clCreateProgramWithSource: " + clErr(err));
  }

  err = clBuildProgram(program, 1, &dev.device, nullptr, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    size_t logSize = 0;
    clGetProgramBuildInfo(program, dev.device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &logSize);
    std::string log(logSize, '\0');
    if (logSize)
      clGetProgramBuildInfo(program, dev.device, CL_PROGRAM_BUILD_LOG, logSize, log.data(), nullptr);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(ctx);
    Error e = Error::make(ErrorCode::CompilationFailed, "clBuildProgram failed: " + log);
    e.backend = "opencl";
    return e;
  }

  const char *kName = isMatmul ? "matmul" : "vector_add";
  cl_kernel kernel = clCreateKernel(program, kName, &err);
  if (err != CL_SUCCESS || !kernel) {
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(ctx);
    return Error::make(ErrorCode::CompilationFailed, "clCreateKernel: " + clErr(err));
  }

  VectorAddHost vecHost;
  MatmulHost matHost;
  cl_mem bufA = nullptr;
  cl_mem bufB = nullptr;
  cl_mem bufC = nullptr;
  size_t outBytes = 0;
  size_t global1 = 0;
  size_t local1 = 256;
  size_t global2[2] = {0, 0};
  size_t local2[2] = {16, 16};
  cl_uint workDim = 1;
  const size_t *globalPtr = nullptr;
  const size_t *localPtr = nullptr;

  if (isMatmul) {
    matHost = makeMatmulHost(requireMatmulFp32(workload, "opencl").value());
    const auto &d = matHost.dims;
    const size_t aBytes = static_cast<size_t>(d.M) * d.K * sizeof(float);
    const size_t bBytes = static_cast<size_t>(d.K) * d.N * sizeof(float);
    outBytes = static_cast<size_t>(d.M) * d.N * sizeof(float);
    bufA = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, aBytes, matHost.a.data(),
                          &err);
    bufB = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, bBytes, matHost.b.data(),
                          &err);
    bufC = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, outBytes, nullptr, &err);
    cl_uint M = d.M, N = d.N, K = d.K;
    clSetKernelArg(kernel, 0, sizeof(cl_mem), &bufA);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &bufB);
    clSetKernelArg(kernel, 2, sizeof(cl_mem), &bufC);
    clSetKernelArg(kernel, 3, sizeof(cl_uint), &M);
    clSetKernelArg(kernel, 4, sizeof(cl_uint), &N);
    clSetKernelArg(kernel, 5, sizeof(cl_uint), &K);
    global2[0] = ((static_cast<size_t>(d.N) + local2[0] - 1) / local2[0]) * local2[0];
    global2[1] = ((static_cast<size_t>(d.M) + local2[1] - 1) / local2[1]) * local2[1];
    workDim = 2;
    globalPtr = global2;
    localPtr = local2;
  } else {
    const uint32_t n = requireVectorAddFp32(workload, "opencl").value();
    vecHost = makeVectorAddHost(n);
    outBytes = static_cast<size_t>(n) * sizeof(float);
    bufA = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, outBytes, vecHost.a.data(),
                          &err);
    bufB = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, outBytes, vecHost.b.data(),
                          &err);
    bufC = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, outBytes, nullptr, &err);
    clSetKernelArg(kernel, 0, sizeof(cl_mem), &bufA);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &bufB);
    clSetKernelArg(kernel, 2, sizeof(cl_mem), &bufC);
    clSetKernelArg(kernel, 3, sizeof(cl_uint), &n);
    global1 = ((static_cast<size_t>(n) + local1 - 1) / local1) * local1;
    workDim = 1;
    globalPtr = &global1;
    localPtr = &local1;
  }

  if (!bufA || !bufB || !bufC) {
    if (bufA)
      clReleaseMemObject(bufA);
    if (bufB)
      clReleaseMemObject(bufB);
    if (bufC)
      clReleaseMemObject(bufC);
    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(ctx);
    return Error::make(ErrorCode::ExecutionFailed, "clCreateBuffer failed");
  }

  const int warmupIters = options.benchmark ? workload.execution.warmup : 0;
  const int timedIters = options.benchmark ? workload.execution.iterations : 1;
  PowerSampler power;
  TimingSource timingSource =
      profilingQueue ? TimingSource::GpuTimestamp : TimingSource::CpuWall;

  auto cleanup = [&]() {
    clReleaseMemObject(bufA);
    clReleaseMemObject(bufB);
    clReleaseMemObject(bufC);
    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(ctx);
  };

  for (int i = 0; i < warmupIters; ++i) {
    auto w = profileOrWall(queue, kernel, workDim, globalPtr, localPtr, false, profilingQueue,
                           timingSource);
    if (!w) {
      cleanup();
      return w.error();
    }
  }

  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(timedIters));
  for (int i = 0; i < timedIters; ++i) {
    auto ms = profileOrWall(queue, kernel, workDim, globalPtr, localPtr, true, profilingQueue,
                            timingSource);
    if (!ms) {
      cleanup();
      return ms.error();
    }
    samples.push_back(ms.value());
  }

  float *outPtr = isMatmul ? matHost.output.data() : vecHost.output.data();
  err = clEnqueueReadBuffer(queue, bufC, CL_TRUE, 0, outBytes, outPtr, 0, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    cleanup();
    return Error::make(ErrorCode::ExecutionFailed, "clEnqueueReadBuffer: " + clErr(err));
  }

  DeviceInfo report = device;
  if (report.name.empty())
    report.name = dev.name;
  if (report.vendor.empty())
    report.vendor = dev.vendor;
  if (report.driver.empty())
    report.driver = dev.driver;

  RunResult result =
      isMatmul ? fillMatmulResult(workload, report, BackendType::OpenCL, "opencl-online-compiler",
                                  options, samples, timingSource, matHost.reference, matHost.output)
               : fillVectorAddResult(workload, report, BackendType::OpenCL, "opencl-online-compiler",
                                     options, samples, timingSource, vecHost.reference,
                                     vecHost.output);
  result.power = power.stats();
  cleanup();
  logInfo("runtime", std::string("OpenCL ") + (isMatmul ? "matmul" : "vector_add") +
                         " completed on " + result.deviceName);
  return result;
}

std::unique_ptr<Backend> createOpenCLBackend() {
  return std::make_unique<OpenCLBackend>();
}

} // namespace csrun
