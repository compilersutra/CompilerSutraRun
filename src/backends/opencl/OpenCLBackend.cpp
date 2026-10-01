#include "csrun/backends/opencl/OpenCLBackend.h"

#include "csrun/benchmark/PowerSampler.h"
#include "csrun/core/Log.h"
#include "csrun/workload/VectorAddScaffold.h"

#define CL_TARGET_OPENCL_VERSION 300
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#include <CL/cl.h>

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
  for (const auto &d : all.value()) {
    if (d.index == want.index || d.name == want.name)
      return d;
  }
  return all.value().front();
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

Expected<RunResult> OpenCLBackend::execute(const Workload &workload, const DeviceInfo &device,
                                           const ExecuteOptions &options) const {
  auto nOr = requireVectorAddFp32(workload, "opencl");
  if (!nOr)
    return nOr.error();
  const uint32_t n = nOr.value();

  static constexpr std::string_view kFallbacks[] = {
      "examples/vector_add/vector_add.cl",
      "../examples/vector_add/vector_add.cl",
      "../../examples/vector_add/vector_add.cl",
  };
  auto kernelPath = resolveKernelPath(workload, "vector_add.cl", kFallbacks);
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

  cl_command_queue queue = clCreateCommandQueueWithProperties(ctx, dev.device, nullptr, &err);
  if (err != CL_SUCCESS || !queue)
    queue = clCreateCommandQueue(ctx, dev.device, 0, &err);
  if (err != CL_SUCCESS || !queue) {
    clReleaseContext(ctx);
    return Error::make(ErrorCode::DriverError, "clCreateCommandQueue: " + clErr(err));
  }

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

  cl_kernel kernel = clCreateKernel(program, "vector_add", &err);
  if (err != CL_SUCCESS || !kernel) {
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(ctx);
    return Error::make(ErrorCode::CompilationFailed, "clCreateKernel: " + clErr(err));
  }

  VectorAddHost host = makeVectorAddHost(n);
  const size_t bytes = static_cast<size_t>(n) * sizeof(float);
  cl_mem bufA =
      clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, bytes, host.a.data(), &err);
  cl_mem bufB =
      clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, bytes, host.b.data(), &err);
  cl_mem bufC = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, bytes, nullptr, &err);
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

  clSetKernelArg(kernel, 0, sizeof(cl_mem), &bufA);
  clSetKernelArg(kernel, 1, sizeof(cl_mem), &bufB);
  clSetKernelArg(kernel, 2, sizeof(cl_mem), &bufC);
  clSetKernelArg(kernel, 3, sizeof(cl_uint), &n);

  const size_t local = 256;
  const size_t global = ((static_cast<size_t>(n) + local - 1) / local) * local;
  const int warmupIters = options.benchmark ? workload.execution.warmup : 0;
  const int timedIters = options.benchmark ? workload.execution.iterations : 1;

  PowerSampler power;

  auto dispatchOnce = [&]() -> Expected<double> {
    const auto t0 = std::chrono::steady_clock::now();
    cl_int e =
        clEnqueueNDRangeKernel(queue, kernel, 1, nullptr, &global, &local, 0, nullptr, nullptr);
    if (e != CL_SUCCESS)
      return Error::make(ErrorCode::ExecutionFailed, "clEnqueueNDRangeKernel: " + clErr(e));
    e = clFinish(queue);
    if (e != CL_SUCCESS)
      return Error::make(ErrorCode::ExecutionFailed, "clFinish: " + clErr(e));
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
  };

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
    auto w = dispatchOnce();
    if (!w) {
      cleanup();
      return w.error();
    }
  }

  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(timedIters));
  for (int i = 0; i < timedIters; ++i) {
    auto ms = dispatchOnce();
    if (!ms) {
      cleanup();
      return ms.error();
    }
    samples.push_back(ms.value());
  }

  err = clEnqueueReadBuffer(queue, bufC, CL_TRUE, 0, bytes, host.output.data(), 0, nullptr, nullptr);
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
      fillVectorAddResult(workload, report, BackendType::OpenCL, "opencl-online-compiler", options,
                          samples, TimingSource::CpuWall, host.reference, host.output);
  result.power = power.stats();
  cleanup();
  logInfo("runtime", "OpenCL vector_add completed on " + result.deviceName);
  return result;
}

std::unique_ptr<Backend> createOpenCLBackend() {
  return std::make_unique<OpenCLBackend>();
}

} // namespace csrun
