#include "csrun/core/Log.h"
#include "csrun/result/RunResult.h"
#include "csrun/runtime/PlatformRuntime.h"

#include <CLI/CLI.hpp>

#include <iostream>
#include <optional>
#include <string>

namespace {

csrun::LogLevel parseLogLevel(const std::string &text) {
  if (text == "error")
    return csrun::LogLevel::Error;
  if (text == "warn")
    return csrun::LogLevel::Warn;
  if (text == "info")
    return csrun::LogLevel::Info;
  if (text == "debug")
    return csrun::LogLevel::Debug;
  if (text == "trace")
    return csrun::LogLevel::Trace;
  return csrun::LogLevel::Info;
}

} // namespace

int main(int argc, char **argv) {
  CLI::App app{"CompilerSutraRun (csrun) — universal GPU workload runner"};

  std::string workloadPath;
  std::string backendName;
  std::string format = "text";
  std::string logLevel = "info";
  bool listBackends = false;
  bool listDevices = false;
  bool inspectOnly = false;
  bool validate = false;
  bool benchmark = false;
  bool allBackends = false;

  app.add_option("workload", workloadPath, "Path to workload YAML");
  app.add_option("--backend", backendName, "Force backend (vulkan|cuda|d3d12|...)");
  app.add_option("--format", format, "Output format: text|json")->check(CLI::IsMember({"text", "json"}));
  app.add_option("--log-level", logLevel, "error|warn|info|debug|trace")
      ->check(CLI::IsMember({"error", "warn", "info", "debug", "trace"}));
  app.add_flag("--list-backends", listBackends, "List registered backends");
  app.add_flag("--list-devices", listDevices, "List devices (Phase 2+)");
  app.add_flag("--inspect", inspectOnly, "Parse and report workload without GPU execute");
  app.add_flag("--validate", validate, "Run validation (Phase 2+)");
  app.add_flag("--benchmark", benchmark, "Run benchmark (Phase 2+)");
  app.add_flag("--all", allBackends, "Run across all available backends (Phase 6)");

  CLI11_PARSE(app, argc, argv);

  csrun::setLogLevel(parseLogLevel(logLevel));
  csrun::PlatformRuntime runtime;

  if (listBackends) {
    std::cout << "CompilerSutraRun backends\n";
    std::cout << "-------------------------\n";
    for (const auto &b : runtime.listBackends()) {
      std::cout << b.name << "\t"
                << (b.available ? "AVAILABLE" : "UNAVAILABLE") << "\t" << b.availabilityNote
                << '\n';
    }
    return 0;
  }

  if (listDevices) {
    std::cout << "DEVICE_NOT_FOUND: device enumeration requires a built backend "
                 "(Vulkan Phase 2)\n";
    return 2;
  }

  if (workloadPath.empty()) {
    std::cerr << "error: workload path required (or use --list-backends)\n";
    return 1;
  }

  auto workload = runtime.loadWorkload(workloadPath);
  if (!workload) {
    std::cerr << workload.error().toString() << '\n';
    return 1;
  }

  std::optional<csrun::BackendType> forced;
  if (!backendName.empty()) {
    forced = csrun::parseBackendType(backendName);
    if (!forced) {
      std::cerr << "INVALID_WORKLOAD: unknown backend '" << backendName << "'\n";
      return 1;
    }
  }

  if (validate || benchmark || allBackends) {
    std::cerr << "UNIMPLEMENTED: --validate/--benchmark/--all require Phase 2+ GPU backends\n";
    return 3;
  }

  // Default Phase 1 behavior: inspect.
  (void)inspectOnly;
  auto result = runtime.inspect(workload.value(), forced);
  if (!result) {
    std::cerr << result.error().toString() << '\n';
    return 1;
  }

  if (format == "json") {
    auto json = csrun::JsonWriter::write(result.value());
    if (!json) {
      std::cerr << json.error().toString() << '\n';
      return 1;
    }
    std::cout << json.value() << '\n';
    return 0;
  }

  const auto &r = result.value();
  std::cout << "CompilerSutraRun\n";
  std::cout << "Workload: " << r.workloadName << " (" << r.operation << ")\n";
  std::cout << "Backend:  " << csrun::toString(r.backend) << '\n';
  std::cout << "Status:   " << r.note << '\n';
  std::cout << "Validation: " << csrun::toString(r.validation) << '\n';
  return 0;
}
