#include "csrun/workload/WorkloadParser.h"

#include "csrun/core/Log.h"

#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace csrun {
namespace {

Expected<Workload> fromNode(const YAML::Node &root, std::string_view sourceName) {
  if (!root || !root.IsMap()) {
    return Error::make(ErrorCode::InvalidWorkload,
                       std::string("workload root must be a mapping: ") + std::string(sourceName));
  }

  Workload w;
  w.sourcePath = std::string(sourceName);

  if (!root["name"] || !root["operation"]) {
    return Error::make(ErrorCode::InvalidWorkload,
                       "workload requires 'name' and 'operation' fields");
  }

  w.name = root["name"].as<std::string>();
  w.operation = root["operation"].as<std::string>();

  if (root["dimensions"]) {
    const auto &d = root["dimensions"];
    if (d["M"])
      w.dimensions.m = d["M"].as<std::uint64_t>();
    if (d["N"])
      w.dimensions.n = d["N"].as<std::uint64_t>();
    if (d["K"])
      w.dimensions.k = d["K"].as<std::uint64_t>();
    if (d["count"])
      w.dimensions.count = d["count"].as<std::uint64_t>();
  }

  if (root["dtype"]) {
    const auto &dt = root["dtype"];
    if (dt.IsScalar()) {
      w.inputDtype = parseDataType(dt.as<std::string>());
      w.outputDtype = w.inputDtype;
    } else {
      if (dt["input"])
        w.inputDtype = parseDataType(dt["input"].as<std::string>());
      if (dt["output"])
        w.outputDtype = parseDataType(dt["output"].as<std::string>());
    }
    if (w.inputDtype == DataType::Unknown || w.outputDtype == DataType::Unknown) {
      return Error::make(ErrorCode::UnsupportedDtype, "unknown dtype in workload");
    }
  }

  if (root["execution"]) {
    const auto &ex = root["execution"];
    if (ex["warmup"])
      w.execution.warmup = ex["warmup"].as<int>();
    if (ex["iterations"])
      w.execution.iterations = ex["iterations"].as<int>();
    if (ex["show"])
      w.execution.show = ex["show"].as<bool>();
    if (ex["show_ms"])
      w.execution.showMs = ex["show_ms"].as<int>();
    if (ex["width"])
      w.execution.width = ex["width"].as<std::uint32_t>();
    if (ex["height"])
      w.execution.height = ex["height"].as<std::uint32_t>();
    if (ex["extent"] && ex["extent"].IsSequence() && ex["extent"].size() >= 2) {
      w.execution.width = ex["extent"][0].as<std::uint32_t>();
      w.execution.height = ex["extent"][1].as<std::uint32_t>();
    }
  }

  if (root["validation"]) {
    const auto &v = root["validation"];
    if (v["enabled"])
      w.validation.enabled = v["enabled"].as<bool>();
    if (v["tolerance"]) {
      w.validation.absoluteTolerance = v["tolerance"].as<double>();
      w.validation.relativeTolerance = v["tolerance"].as<double>();
    }
    if (v["absolute_tolerance"])
      w.validation.absoluteTolerance = v["absolute_tolerance"].as<double>();
    if (v["relative_tolerance"])
      w.validation.relativeTolerance = v["relative_tolerance"].as<double>();
  }

  if (root["backend"]) {
    auto parsed = parseBackendType(root["backend"].as<std::string>());
    if (!parsed) {
      return Error::make(ErrorCode::InvalidWorkload, "unknown preferred backend");
    }
    w.preferredBackend = *parsed;
  }

  auto addShader = [&](const YAML::Node &n) -> Expected<void> {
    ShaderRef ref;
    if (n.IsScalar()) {
      ref.path = n.as<std::string>();
    } else if (n.IsMap()) {
      if (!n["path"])
        return Error::make(ErrorCode::InvalidWorkload, "shaders[] entry requires path");
      ref.path = n["path"].as<std::string>();
      if (n["stage"])
        ref.stage = n["stage"].as<std::string>();
    } else {
      return Error::make(ErrorCode::InvalidWorkload, "invalid shaders[] entry");
    }
    w.shaders.push_back(std::move(ref));
    return {};
  };

  if (root["shaders"]) {
    const auto &s = root["shaders"];
    if (s.IsSequence()) {
      for (const auto &item : s) {
        auto ok = addShader(item);
        if (!ok)
          return ok.error();
      }
    } else if (s.IsMap()) {
      // shaders: { vertex: a.vert, fragment: b.frag, compute: c.comp }
      for (auto it = s.begin(); it != s.end(); ++it) {
        ShaderRef ref;
        ref.stage = it->first.as<std::string>();
        ref.path = it->second.as<std::string>();
        w.shaders.push_back(std::move(ref));
      }
    } else {
      return Error::make(ErrorCode::InvalidWorkload, "shaders must be a list or map");
    }
  }

  if (root["spv_output_dir"])
    w.spvOutputDir = root["spv_output_dir"].as<std::string>();

  if (isSpirvWorkload(w) && w.shaders.empty()) {
    return Error::make(ErrorCode::InvalidWorkload,
                       "spirv/shader workload requires shaders: [.vert/.frag/.comp, ...]");
  }

  if (w.execution.iterations < 1) {
    return Error::make(ErrorCode::InvalidWorkload, "execution.iterations must be >= 1");
  }

  logDebug("workload", "parsed workload '" + w.name + "'");
  return w;
}

} // namespace

Expected<Workload> WorkloadParser::parseString(std::string_view yamlText,
                                               std::string_view sourceName) {
  try {
    YAML::Node root = YAML::Load(std::string(yamlText));
    return fromNode(root, sourceName);
  } catch (const YAML::Exception &ex) {
    return Error::make(ErrorCode::InvalidWorkload,
                       std::string("YAML parse error: ") + ex.what());
  }
}

Expected<Workload> WorkloadParser::parseFile(const std::filesystem::path &path) {
  std::ifstream in(path);
  if (!in) {
    return Error::make(ErrorCode::IoError, "failed to open workload file: " + path.string());
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return parseString(ss.str(), path.string());
}

} // namespace csrun
