#include "csrun/backends/opengl/OpenGLBackend.h"

#include "csrun/benchmark/PowerSampler.h"
#include "csrun/core/Log.h"
#include "csrun/workload/MatmulScaffold.h"
#include "csrun/workload/VectorAddScaffold.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#define GL_GLEXT_PROTOTYPES 0
#include <GL/glcorearb.h>

#include <chrono>
#include <memory>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

namespace csrun {
namespace {

struct GlApi {
  PFNGLGETSTRINGPROC GetString = nullptr;
  PFNGLGETERRORPROC GetError = nullptr;
  PFNGLCREATESHADERPROC CreateShader = nullptr;
  PFNGLSHADERSOURCEPROC ShaderSource = nullptr;
  PFNGLCOMPILESHADERPROC CompileShader = nullptr;
  PFNGLGETSHADERIVPROC GetShaderiv = nullptr;
  PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog = nullptr;
  PFNGLCREATEPROGRAMPROC CreateProgram = nullptr;
  PFNGLATTACHSHADERPROC AttachShader = nullptr;
  PFNGLLINKPROGRAMPROC LinkProgram = nullptr;
  PFNGLGETPROGRAMIVPROC GetProgramiv = nullptr;
  PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog = nullptr;
  PFNGLDELETESHADERPROC DeleteShader = nullptr;
  PFNGLDELETEPROGRAMPROC DeleteProgram = nullptr;
  PFNGLUSEPROGRAMPROC UseProgram = nullptr;
  PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation = nullptr;
  PFNGLUNIFORM1UIPROC Uniform1ui = nullptr;
  PFNGLGENBUFFERSPROC GenBuffers = nullptr;
  PFNGLBINDBUFFERPROC BindBuffer = nullptr;
  PFNGLBUFFERDATAPROC BufferData = nullptr;
  PFNGLBINDBUFFERBASEPROC BindBufferBase = nullptr;
  PFNGLDELETEBUFFERSPROC DeleteBuffers = nullptr;
  PFNGLMAPBUFFERRANGEPROC MapBufferRange = nullptr;
  PFNGLUNMAPBUFFERPROC UnmapBuffer = nullptr;
  PFNGLDISPATCHCOMPUTEPROC DispatchCompute = nullptr;
  PFNGLMEMORYBARRIERPROC MemoryBarrier = nullptr;
  PFNGLFINISHPROC Finish = nullptr;
  PFNGLGENQUERIESPROC GenQueries = nullptr;
  PFNGLDELETEQUERIESPROC DeleteQueries = nullptr;
  PFNGLBEGINQUERYPROC BeginQuery = nullptr;
  PFNGLENDQUERYPROC EndQuery = nullptr;
  PFNGLGETQUERYOBJECTUI64VPROC GetQueryObjectui64v = nullptr;
};

template <typename T>
T load(const char *name) {
  return reinterpret_cast<T>(eglGetProcAddress(name));
}

bool loadGl(GlApi &gl) {
  gl.GetString = load<PFNGLGETSTRINGPROC>("glGetString");
  gl.GetError = load<PFNGLGETERRORPROC>("glGetError");
  gl.CreateShader = load<PFNGLCREATESHADERPROC>("glCreateShader");
  gl.ShaderSource = load<PFNGLSHADERSOURCEPROC>("glShaderSource");
  gl.CompileShader = load<PFNGLCOMPILESHADERPROC>("glCompileShader");
  gl.GetShaderiv = load<PFNGLGETSHADERIVPROC>("glGetShaderiv");
  gl.GetShaderInfoLog = load<PFNGLGETSHADERINFOLOGPROC>("glGetShaderInfoLog");
  gl.CreateProgram = load<PFNGLCREATEPROGRAMPROC>("glCreateProgram");
  gl.AttachShader = load<PFNGLATTACHSHADERPROC>("glAttachShader");
  gl.LinkProgram = load<PFNGLLINKPROGRAMPROC>("glLinkProgram");
  gl.GetProgramiv = load<PFNGLGETPROGRAMIVPROC>("glGetProgramiv");
  gl.GetProgramInfoLog = load<PFNGLGETPROGRAMINFOLOGPROC>("glGetProgramInfoLog");
  gl.DeleteShader = load<PFNGLDELETESHADERPROC>("glDeleteShader");
  gl.DeleteProgram = load<PFNGLDELETEPROGRAMPROC>("glDeleteProgram");
  gl.UseProgram = load<PFNGLUSEPROGRAMPROC>("glUseProgram");
  gl.GetUniformLocation = load<PFNGLGETUNIFORMLOCATIONPROC>("glGetUniformLocation");
  gl.Uniform1ui = load<PFNGLUNIFORM1UIPROC>("glUniform1ui");
  gl.GenBuffers = load<PFNGLGENBUFFERSPROC>("glGenBuffers");
  gl.BindBuffer = load<PFNGLBINDBUFFERPROC>("glBindBuffer");
  gl.BufferData = load<PFNGLBUFFERDATAPROC>("glBufferData");
  gl.BindBufferBase = load<PFNGLBINDBUFFERBASEPROC>("glBindBufferBase");
  gl.DeleteBuffers = load<PFNGLDELETEBUFFERSPROC>("glDeleteBuffers");
  gl.MapBufferRange = load<PFNGLMAPBUFFERRANGEPROC>("glMapBufferRange");
  gl.UnmapBuffer = load<PFNGLUNMAPBUFFERPROC>("glUnmapBuffer");
  gl.DispatchCompute = load<PFNGLDISPATCHCOMPUTEPROC>("glDispatchCompute");
  gl.MemoryBarrier = load<PFNGLMEMORYBARRIERPROC>("glMemoryBarrier");
  gl.Finish = load<PFNGLFINISHPROC>("glFinish");
  gl.GenQueries = load<PFNGLGENQUERIESPROC>("glGenQueries");
  gl.DeleteQueries = load<PFNGLDELETEQUERIESPROC>("glDeleteQueries");
  gl.BeginQuery = load<PFNGLBEGINQUERYPROC>("glBeginQuery");
  gl.EndQuery = load<PFNGLENDQUERYPROC>("glEndQuery");
  gl.GetQueryObjectui64v = load<PFNGLGETQUERYOBJECTUI64VPROC>("glGetQueryObjectui64v");
  return gl.GetString && gl.CreateShader && gl.DispatchCompute && gl.BufferData &&
         gl.MapBufferRange && gl.Finish;
}

struct EglContext {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface surface = EGL_NO_SURFACE;
  EGLConfig config = nullptr;
  GlApi gl{};

  ~EglContext() { destroy(); }

  void destroy() {
    if (display != EGL_NO_DISPLAY) {
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (context != EGL_NO_CONTEXT)
        eglDestroyContext(display, context);
      if (surface != EGL_NO_SURFACE)
        eglDestroySurface(display, surface);
      eglTerminate(display);
    }
    display = EGL_NO_DISPLAY;
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;
  }
};

Expected<std::unique_ptr<EglContext>> createHeadlessGl() {
  auto ctx = std::make_unique<EglContext>();

  auto getPlatform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (!getPlatform) {
    getPlatform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplay"));
  }
  if (!getPlatform)
    return Error::make(ErrorCode::DriverError, "eglGetPlatformDisplay(EXT) unavailable");

  ctx->display = getPlatform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
  if (ctx->display == EGL_NO_DISPLAY)
    return Error::make(ErrorCode::DriverError, "EGL surfaceless display unavailable");

  if (!eglInitialize(ctx->display, nullptr, nullptr))
    return Error::make(ErrorCode::DriverError, "eglInitialize failed");
  if (!eglBindAPI(EGL_OPENGL_API))
    return Error::make(ErrorCode::DriverError, "eglBindAPI(OPENGL) failed");

  const EGLint cfgAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE,
                             EGL_PBUFFER_BIT, EGL_NONE};
  EGLint n = 0;
  if (!eglChooseConfig(ctx->display, cfgAttrs, &ctx->config, 1, &n) || n < 1)
    return Error::make(ErrorCode::DriverError, "eglChooseConfig failed");

  const EGLint ctxAttrs[] = {EGL_CONTEXT_MAJOR_VERSION,
                             4,
                             EGL_CONTEXT_MINOR_VERSION,
                             3,
                             EGL_CONTEXT_OPENGL_PROFILE_MASK,
                             EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                             EGL_NONE};
  ctx->context = eglCreateContext(ctx->display, ctx->config, EGL_NO_CONTEXT, ctxAttrs);
  if (ctx->context == EGL_NO_CONTEXT)
    return Error::make(ErrorCode::DriverError, "eglCreateContext GL 4.3 core failed");

  const EGLint pbufAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  ctx->surface = eglCreatePbufferSurface(ctx->display, ctx->config, pbufAttrs);
  if (ctx->surface == EGL_NO_SURFACE)
    return Error::make(ErrorCode::DriverError, "eglCreatePbufferSurface failed");

  if (!eglMakeCurrent(ctx->display, ctx->surface, ctx->surface, ctx->context))
    return Error::make(ErrorCode::DriverError, "eglMakeCurrent failed");

  if (!loadGl(ctx->gl))
    return Error::make(ErrorCode::DriverError, "failed to load required GL entry points");

  return ctx;
}

Expected<std::string> readFile(const std::filesystem::path &path) {
  std::ifstream in(path);
  if (!in)
    return Error::make(ErrorCode::IoError, "failed to open shader: " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

Expected<GLuint> buildComputeProgram(GlApi &gl, const std::string &src) {
  GLuint shader = gl.CreateShader(GL_COMPUTE_SHADER);
  const char *c = src.c_str();
  gl.ShaderSource(shader, 1, &c, nullptr);
  gl.CompileShader(shader);
  GLint ok = 0;
  gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    gl.GetShaderInfoLog(shader, sizeof(log), nullptr, log);
    gl.DeleteShader(shader);
    Error e = Error::make(ErrorCode::CompilationFailed, std::string("GLSL compile failed: ") + log);
    e.backend = "opengl";
    return e;
  }
  GLuint prog = gl.CreateProgram();
  gl.AttachShader(prog, shader);
  gl.LinkProgram(prog);
  gl.DeleteShader(shader);
  gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    gl.GetProgramInfoLog(prog, sizeof(log), nullptr, log);
    gl.DeleteProgram(prog);
    Error e = Error::make(ErrorCode::CompilationFailed, std::string("GLSL link failed: ") + log);
    e.backend = "opengl";
    return e;
  }
  return prog;
}

} // namespace

BackendInfo OpenGLBackend::info() const {
  BackendInfo i;
  i.type = BackendType::OpenGL;
  i.name = "opengl";
  auto devices = enumerateDevices();
  if (devices && !devices.value().empty()) {
    i.available = true;
    i.availabilityNote = devices.value().front().name;
  } else {
    i.available = false;
    i.availabilityNote = devices ? "no OpenGL device" : devices.error().message;
  }
  return i;
}

Expected<std::vector<DeviceInfo>> OpenGLBackend::enumerateDevices() const {
  auto ctx = createHeadlessGl();
  if (!ctx)
    return ctx.error();
  auto &gl = ctx.value()->gl;
  DeviceInfo d;
  d.backend = BackendType::OpenGL;
  d.index = 0;
  d.name = gl.GetString ? reinterpret_cast<const char *>(gl.GetString(GL_RENDERER)) : "OpenGL";
  d.vendor = gl.GetString ? reinterpret_cast<const char *>(gl.GetString(GL_VENDOR)) : "";
  d.driver = gl.GetString ? reinterpret_cast<const char *>(gl.GetString(GL_VERSION)) : "";
  d.available = true;
  return std::vector<DeviceInfo>{std::move(d)};
}

Expected<RunResult> OpenGLBackend::execute(const Workload &workload,
                                           const DeviceInfo &device,
                                           const ExecuteOptions &options) const {
  const bool isMatmul = workload.operation == "matmul";
  MatmulDims matDims{};
  uint32_t n = 0;
  if (isMatmul) {
    auto dOr = requireMatmulFp32(workload, "opengl");
    if (!dOr)
      return dOr.error();
    matDims = dOr.value();
  } else {
    auto nOr = requireVectorAddFp32(workload, "opengl");
    if (!nOr)
      return nOr.error();
    n = nOr.value();
  }

  static constexpr std::string_view kVecFallbacks[] = {
      "examples/vector_add/vector_add.glsl",
      "../examples/vector_add/vector_add.glsl",
      "../../examples/vector_add/vector_add.glsl",
  };
  static constexpr std::string_view kMatFallbacks[] = {
      "examples/matmul/matmul.glsl",
      "../examples/matmul/matmul.glsl",
      "../../examples/matmul/matmul.glsl",
  };
  auto shaderPath = isMatmul ? resolveKernelPath(workload, "matmul.glsl", kMatFallbacks)
                             : resolveKernelPath(workload, "vector_add.glsl", kVecFallbacks);
  if (!shaderPath)
    return shaderPath.error();
  auto src = readFile(shaderPath.value());
  if (!src)
    return src.error();

  auto ctx = createHeadlessGl();
  if (!ctx)
    return ctx.error();
  GlApi &gl = ctx.value()->gl;

  auto prog = buildComputeProgram(gl, src.value());
  if (!prog)
    return prog.error();

  VectorAddHost vecHost;
  MatmulHost matHost;
  GLsizeiptr bytesA = 0, bytesB = 0, bytesC = 0;
  if (isMatmul) {
    matHost = makeMatmulHost(matDims);
    bytesA = static_cast<GLsizeiptr>(matDims.M * matDims.K * sizeof(float));
    bytesB = static_cast<GLsizeiptr>(matDims.K * matDims.N * sizeof(float));
    bytesC = static_cast<GLsizeiptr>(matDims.M * matDims.N * sizeof(float));
  } else {
    vecHost = makeVectorAddHost(n);
    bytesA = bytesB = bytesC = static_cast<GLsizeiptr>(n * sizeof(float));
  }
  const int warmupIters = options.benchmark ? workload.execution.warmup : 0;
  const int timedIters = options.benchmark ? workload.execution.iterations : 1;

  PowerSampler power;

  GLuint buffers[3] = {};
  gl.GenBuffers(3, buffers);
  const GLsizeiptr sizes[3] = {bytesA, bytesB, bytesC};
  for (int i = 0; i < 3; ++i) {
    gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[i]);
    gl.BufferData(GL_SHADER_STORAGE_BUFFER, sizes[i], nullptr, GL_DYNAMIC_COPY);
    gl.BindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(i), buffers[i]);
  }

  gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[0]);
  void *mapped = gl.MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, bytesA, GL_MAP_WRITE_BIT);
  std::memcpy(mapped, isMatmul ? matHost.a.data() : vecHost.a.data(), static_cast<size_t>(bytesA));
  gl.UnmapBuffer(GL_SHADER_STORAGE_BUFFER);
  gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[1]);
  mapped = gl.MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, bytesB, GL_MAP_WRITE_BIT);
  std::memcpy(mapped, isMatmul ? matHost.b.data() : vecHost.b.data(), static_cast<size_t>(bytesB));
  gl.UnmapBuffer(GL_SHADER_STORAGE_BUFFER);

  gl.UseProgram(prog.value());
  if (isMatmul) {
    const GLint locM = gl.GetUniformLocation(prog.value(), "uM");
    const GLint locN = gl.GetUniformLocation(prog.value(), "uN");
    const GLint locK = gl.GetUniformLocation(prog.value(), "uK");
    if (locM >= 0)
      gl.Uniform1ui(locM, matDims.M);
    if (locN >= 0)
      gl.Uniform1ui(locN, matDims.N);
    if (locK >= 0)
      gl.Uniform1ui(locK, matDims.K);
  } else {
    const GLint loc = gl.GetUniformLocation(prog.value(), "uCount");
    if (loc >= 0)
      gl.Uniform1ui(loc, n);
  }

  const bool haveTimer = gl.GenQueries && gl.BeginQuery && gl.EndQuery && gl.GetQueryObjectui64v;
  GLuint query = 0;
  if (haveTimer)
    gl.GenQueries(1, &query);

  auto dispatchOnce = [&](bool timed) -> Expected<double> {
    if (timed && haveTimer)
      gl.BeginQuery(GL_TIME_ELAPSED, query);
    const auto t0 = std::chrono::steady_clock::now();
    if (isMatmul) {
      const uint32_t gx = (matDims.N + 15u) / 16u;
      const uint32_t gy = (matDims.M + 15u) / 16u;
      gl.DispatchCompute(gx, gy, 1);
    } else {
      const uint32_t groups = (n + 255u) / 256u;
      gl.DispatchCompute(groups, 1, 1);
    }
    gl.MemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    gl.Finish();
    const auto t1 = std::chrono::steady_clock::now();
    if (timed && haveTimer) {
      gl.EndQuery(GL_TIME_ELAPSED);
      GLuint64 ns = 0;
      gl.GetQueryObjectui64v(query, GL_QUERY_RESULT, &ns);
      return static_cast<double>(ns) / 1.0e6;
    }
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
  };

  for (int i = 0; i < warmupIters; ++i) {
    auto w = dispatchOnce(false);
    if (!w) {
      if (query)
        gl.DeleteQueries(1, &query);
      gl.DeleteBuffers(3, buffers);
      gl.DeleteProgram(prog.value());
      return w.error();
    }
  }

  std::vector<double> samples;
  samples.reserve(static_cast<size_t>(timedIters));
  TimingSource timingSource = haveTimer ? TimingSource::GpuTimestamp : TimingSource::CpuWall;
  for (int i = 0; i < timedIters; ++i) {
    auto ms = dispatchOnce(true);
    if (!ms) {
      if (query)
        gl.DeleteQueries(1, &query);
      gl.DeleteBuffers(3, buffers);
      gl.DeleteProgram(prog.value());
      return ms.error();
    }
    samples.push_back(ms.value());
  }

  gl.BindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[2]);
  mapped = gl.MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, bytesC, GL_MAP_READ_BIT);
  std::memcpy(isMatmul ? matHost.output.data() : vecHost.output.data(), mapped,
              static_cast<size_t>(bytesC));
  gl.UnmapBuffer(GL_SHADER_STORAGE_BUFFER);

  RunResult result =
      isMatmul ? fillMatmulResult(workload, device, BackendType::OpenGL, "glsl-opengl-driver",
                                  options, samples, timingSource, matHost.reference, matHost.output)
               : fillVectorAddResult(workload, device, BackendType::OpenGL, "glsl-opengl-driver",
                                     options, samples, timingSource, vecHost.reference,
                                     vecHost.output);
  result.power = power.stats();

  if (query)
    gl.DeleteQueries(1, &query);
  gl.DeleteBuffers(3, buffers);
  gl.DeleteProgram(prog.value());

  logInfo("runtime", std::string("OpenGL ") + (isMatmul ? "matmul" : "vector_add") +
                         " completed on " + result.deviceName);
  return result;
}

std::unique_ptr<Backend> createOpenGLBackend() {
  return std::make_unique<OpenGLBackend>();
}

} // namespace csrun
