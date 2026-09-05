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

void printTextResult(const csrun::RunResult &r) {
  std::cout << "CompilerSutraRun\n";
  std::cout << "Workload:    " << r.workloadName << " (" << r.operation << ")\n";
  std::cout << "Backend:     " << csrun::toString(r.backend) << '\n';
  std::cout << "Device:      " << r.deviceName << '\n';
  std::cout << "Compiler:    " << r.compilerName << '\n';
  std::cout << "Timing:      median=" << r.timing.medianMs << " ms"
            << " source=" << csrun::toString(r.timing.source) << '\n';
  std::cout << "Validation:  " << csrun::toString(r.validation) << '\n';
  if (!r.note.empty())
    std::cout << "Note:        " << r.note << '\n';
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
  app.add_flag("--list-devices", listDevices, "List devices from available backends");
  app.add_flag("--inspect", inspectOnly, "Parse and report workload without GPU execute");
  app.add_flag("--validate", validate, "Enable validation (default on for run)");
  app.add_flag("--benchmark", benchmark, "Enable benchmarking (default on for run)");
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

  std::optional<csrun::BackendType> forced;
  if (!backendName.empty()) {
    forced = csrun::parseBackendType(backendName);
    if (!forced) {
      std::cerr << "INVALID_WORKLOAD: unknown backend '" << backendName << "'\n";
      return 1;
    }
  }

  if (listDevices) {
    auto devices = runtime.listDevices(forced);
    if (!devices) {
      std::cerr << devices.error().toString() << '\n';
      return 2;
    }
    std::cout << "Backend\tIndex\tDevice\n";
    for (const auto &d : devices.value()) {
      std::cout << csrun::toString(d.backend) << '\t' << d.index << '\t' << d.name << '\n';
    }
    return 0;
  }

  if (workloadPath.empty()) {
    std::cerr << "error: workload path required (or use --list-backends/--list-devices)\n";
    return 1;
  }

  auto workload = runtime.loadWorkload(workloadPath);
  if (!workload) {
    std::cerr << workload.error().toString() << '\n';
    return 1;
  }

  if (allBackends) {
    std::cerr << "UNIMPLEMENTED: --all lands in Phase 6\n";
    return 3;
  }

  (void)validate;
  (void)benchmark;

  csrun::Expected<csrun::RunResult> result =
      inspectOnly ? runtime.inspect(workload.value(), forced)
                  : runtime.run(workload.value(), forced);
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
  } else {
    printTextResult(result.value());
  }

  if (result.value().validation == csrun::ValidationStatus::Failed)
    return 4;
  return 0;
}
