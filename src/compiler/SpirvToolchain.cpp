#include "csrun/compiler/SpirvToolchain.h"

#include "csrun/core/Log.h"
#include "csrun/core/Process.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <unistd.h>

namespace csrun {
namespace {

Expected<std::filesystem::path> findGlslang() {
  if (std::filesystem::exists("/usr/bin/glslangValidator"))
    return std::filesystem::path("/usr/bin/glslangValidator");
  auto probe = runProcess({"which", "glslangValidator"});
  if (probe && probe.value().exitCode == 0) {
    std::string path = probe.value().stdoutText;
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
      path.pop_back();
    if (!path.empty())
      return std::filesystem::path(path);
  }
  return Error::make(ErrorCode::ToolchainNotFound,
                     "glslangValidator not found (install glslang-tools)");
}

Expected<std::filesystem::path> findSpirvVal() {
  if (std::filesystem::exists("/usr/bin/spirv-val"))
    return std::filesystem::path("/usr/bin/spirv-val");
  auto probe = runProcess({"which", "spirv-val"});
  if (probe && probe.value().exitCode == 0) {
    std::string path = probe.value().stdoutText;
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
      path.pop_back();
    if (!path.empty())
      return std::filesystem::path(path);
  }
  return Error::make(ErrorCode::ToolchainNotFound, "spirv-val not found (install spirv-tools)");
}

std::string lowerExt(const std::filesystem::path &path) {
  std::string e = path.extension().string();
  for (char &c : e)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

} // namespace

std::string_view toString(ShaderStage stage) {
  switch (stage) {
  case ShaderStage::Unknown:
    return "unknown";
  case ShaderStage::Compute:
    return "compute";
  case ShaderStage::Vertex:
    return "vertex";
  case ShaderStage::Fragment:
    return "fragment";
  case ShaderStage::TessControl:
    return "tesscontrol";
  case ShaderStage::TessEvaluation:
    return "tesseval";
  case ShaderStage::Geometry:
    return "geometry";
  case ShaderStage::RayGen:
    return "raygen";
  case ShaderStage::AnyHit:
    return "anyhit";
  case ShaderStage::ClosestHit:
    return "closesthit";
  case ShaderStage::Miss:
    return "miss";
  case ShaderStage::Intersection:
    return "intersection";
  case ShaderStage::Callable:
    return "callable";
  case ShaderStage::Mesh:
    return "mesh";
  case ShaderStage::Task:
    return "task";
  }
  return "unknown";
}

ShaderStage shaderStageFromPath(const std::filesystem::path &path) {
  const std::string e = lowerExt(path);
  if (e == ".comp")
    return ShaderStage::Compute;
  if (e == ".vert")
    return ShaderStage::Vertex;
  if (e == ".frag")
    return ShaderStage::Fragment;
  if (e == ".tesc")
    return ShaderStage::TessControl;
  if (e == ".tese")
    return ShaderStage::TessEvaluation;
  if (e == ".geom")
    return ShaderStage::Geometry;
  if (e == ".rgen")
    return ShaderStage::RayGen;
  if (e == ".rahit")
    return ShaderStage::AnyHit;
  if (e == ".rchit")
    return ShaderStage::ClosestHit;
  if (e == ".rmiss")
    return ShaderStage::Miss;
  if (e == ".rint")
    return ShaderStage::Intersection;
  if (e == ".rcall")
    return ShaderStage::Callable;
  if (e == ".mesh")
    return ShaderStage::Mesh;
  if (e == ".task")
    return ShaderStage::Task;
  // Allow foo.vert.glsl style
  const auto stem = path.stem();
  if (stem.has_extension())
    return shaderStageFromPath(stem);
  return ShaderStage::Unknown;
}

ShaderStage parseShaderStage(std::string_view text) {
  std::string t(text);
  for (char &c : t)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (t == "comp" || t == "compute")
    return ShaderStage::Compute;
  if (t == "vert" || t == "vertex")
    return ShaderStage::Vertex;
  if (t == "frag" || t == "fragment")
    return ShaderStage::Fragment;
  if (t == "tesc" || t == "tesscontrol" || t == "tess_control")
    return ShaderStage::TessControl;
  if (t == "tese" || t == "tesseval" || t == "tess_evaluation")
    return ShaderStage::TessEvaluation;
  if (t == "geom" || t == "geometry")
    return ShaderStage::Geometry;
  if (t == "rgen" || t == "raygen")
    return ShaderStage::RayGen;
  if (t == "rahit" || t == "anyhit")
    return ShaderStage::AnyHit;
  if (t == "rchit" || t == "closesthit")
    return ShaderStage::ClosestHit;
  if (t == "rmiss" || t == "miss")
    return ShaderStage::Miss;
  if (t == "rint" || t == "intersection")
    return ShaderStage::Intersection;
  if (t == "rcall" || t == "callable")
    return ShaderStage::Callable;
  if (t == "mesh")
    return ShaderStage::Mesh;
  if (t == "task")
    return ShaderStage::Task;
  return ShaderStage::Unknown;
}

std::string_view glslangStageToken(ShaderStage stage) {
  switch (stage) {
  case ShaderStage::Compute:
    return "comp";
  case ShaderStage::Vertex:
    return "vert";
  case ShaderStage::Fragment:
    return "frag";
  case ShaderStage::TessControl:
    return "tesc";
  case ShaderStage::TessEvaluation:
    return "tese";
  case ShaderStage::Geometry:
    return "geom";
  case ShaderStage::RayGen:
    return "rgen";
  case ShaderStage::AnyHit:
    return "rahit";
  case ShaderStage::ClosestHit:
    return "rchit";
  case ShaderStage::Miss:
    return "rmiss";
  case ShaderStage::Intersection:
    return "rint";
  case ShaderStage::Callable:
    return "rcall";
  case ShaderStage::Mesh:
    return "mesh";
  case ShaderStage::Task:
    return "task";
  case ShaderStage::Unknown:
    return "";
  }
  return "";
}

Expected<std::vector<uint32_t>> SpirvToolchain::loadSpvFile(const std::filesystem::path &spvPath) {
  std::ifstream in(spvPath, std::ios::binary);
  if (!in)
    return Error::make(ErrorCode::IoError, "failed to open SPIR-V: " + spvPath.string());
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.size() < 20 || bytes.size() % 4 != 0)
    return Error::make(ErrorCode::CompilationFailed, "invalid SPIR-V size: " + spvPath.string());
  uint32_t magic = 0;
  std::memcpy(&magic, bytes.data(), 4);
  if (magic != 0x07230203u)
    return Error::make(ErrorCode::CompilationFailed, "not a SPIR-V binary: " + spvPath.string());
  std::vector<uint32_t> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  return words;
}

Expected<void> SpirvToolchain::validateSpvFile(const std::filesystem::path &spvPath) {
  auto spirvVal = findSpirvVal();
  if (!spirvVal)
    return spirvVal.error();
  auto validate = runProcess({spirvVal.value().string(), spvPath.string()});
  if (!validate)
    return validate.error();
  if (validate.value().exitCode != 0) {
    Error e = Error::make(ErrorCode::CompilationFailed,
                          "spirv-val failed: " + validate.value().stderrText);
    e.operation = "spirv-validate";
    return e;
  }
  return {};
}

Expected<SpirvCompileResult> SpirvToolchain::compile(const SpirvCompileRequest &req) {
  if (!std::filesystem::exists(req.input))
    return Error::make(ErrorCode::IoError, "shader not found: " + req.input.string());

  const ShaderStage stage = req.stageOverride != ShaderStage::Unknown
                                ? req.stageOverride
                                : shaderStageFromPath(req.input);
  const auto token = glslangStageToken(stage);
  if (token.empty()) {
    return Error::make(ErrorCode::UnsupportedOperation,
                       "unknown shader stage for " + req.input.string() +
                           " (use .comp/.vert/.frag/...)");
  }

  auto glslang = findGlslang();
  if (!glslang)
    return glslang.error();

  SpirvCompileResult out;
  out.stage = stage;
  out.compiler = "glslangValidator";
  out.spvPath = req.output.empty() ? std::filesystem::path(req.input.string() + ".spv")
                                   : req.output;

  if (out.spvPath.has_parent_path())
    std::filesystem::create_directories(out.spvPath.parent_path());

  auto compile = runProcess({glslang.value().string(), "-V", "-S", std::string(token),
                             req.input.string(), "-o", out.spvPath.string()});
  if (!compile)
    return compile.error();
  if (compile.value().exitCode != 0) {
    Error e = Error::make(ErrorCode::CompilationFailed,
                          "glslangValidator failed for " + req.input.string() + ": " +
                              compile.value().stderrText + compile.value().stdoutText);
    e.backend = "vulkan";
    e.operation = "compile";
    e.context = std::string(toString(stage));
    return e;
  }

  if (req.validate) {
    auto v = validateSpvFile(out.spvPath);
    if (!v)
      return v.error();
  }

  auto words = loadSpvFile(out.spvPath);
  if (!words)
    return words.error();
  out.words = std::move(words.value());

  if (!req.keepOutput)
    std::filesystem::remove(out.spvPath);

  logInfo("compiler", std::string("SPIR-V OK stage=") + std::string(toString(stage)) +
                          " words=" + std::to_string(out.words.size()) +
                          " out=" + out.spvPath.string());
  return out;
}

Expected<SpirvModuleInfo> SpirvToolchain::inspectSpvFile(const std::filesystem::path &spvPath) {
  auto words = loadSpvFile(spvPath);
  if (!words)
    return words.error();
  if (words.value().size() < 5)
    return Error::make(ErrorCode::CompilationFailed, "SPIR-V too short to inspect");

  SpirvModuleInfo info;
  info.path = spvPath;
  info.version = words.value()[1];
  info.generator = words.value()[2];
  info.bound = words.value()[3];
  info.wordCount = words.value().size();
  const unsigned major = (info.version >> 16) & 0xffu;
  const unsigned minor = (info.version >> 8) & 0xffu;
  info.summary = "spirv " + std::to_string(major) + "." + std::to_string(minor) +
                 " words=" + std::to_string(info.wordCount) +
                 " bound=" + std::to_string(info.bound) + " file=" + spvPath.string();
  return info;
}

Expected<std::string> SpirvToolchain::disassembleSpvFile(const std::filesystem::path &spvPath,
                                                         int maxLines) {
  auto probe = runProcess({"which", "spirv-dis"});
  if (!probe || probe.value().exitCode != 0)
    return std::string{};
  std::string tool = probe.value().stdoutText;
  while (!tool.empty() && (tool.back() == '\n' || tool.back() == '\r'))
    tool.pop_back();
  auto run = runProcess({tool, spvPath.string()});
  if (!run || run.value().exitCode != 0)
    return std::string{};
  std::string text = run.value().stdoutText;
  if (maxLines <= 0)
    return text;
  std::string out;
  int lines = 0;
  for (size_t i = 0; i < text.size() && lines < maxLines; ++i) {
    out.push_back(text[i]);
    if (text[i] == '\n')
      ++lines;
  }
  if (lines >= maxLines)
    out += "...\n";
  return out;
}

} // namespace csrun