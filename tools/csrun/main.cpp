#include "csrun/core/Log.h"
#include "csrun/compiler/SpirvToolchain.h"
#include "csrun/result/RunResult.h"
#include "csrun/runtime/PlatformRuntime.h"

#if defined(CSRUN_HAS_VULKAN)
#include "csrun/backends/vulkan/VulkanSpirvRunner.h"
#endif

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
  if (r.power.available)
    std::cout << "Power:       avg=" << r.power.avgWatts << " W"
              << " min=" << r.power.minWatts << " W"
              << " max=" << r.power.maxWatts << " W"
              << " energy=" << r.power.energyJoules << " J"
              << " source=" << r.power.source << " (" << r.power.device << ")\n";
  if (r.counters.available || r.counters.occlusionAvailable || r.counters.gpuTimestampAvailable ||
      r.counters.hostRecordMs > 0.0 || r.counters.hostSubmitMs > 0.0 ||
      r.counters.hostQueueWaitMs > 0.0 || !r.counters.vendor.empty() ||
      !r.counters.vendorNote.empty()) {
    std::cout << "Counters:    " << r.counters.summaryLine() << '\n';
    std::cout << r.counters.checklistReport();
  }
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
  std::string compileSpvIn;
  std::string compileSpvOut;
  std::string smokeComputeSpv;
  std::string smokeVertSpv;
  std::string smokeFragSpv;
  std::string inspectSpvPath;
  std::uint32_t deviceIndex = 0;

  bool listBackends = false;
  bool listDevices = false;
  bool inspectOnly = false;
  bool validate = false;
  bool benchmark = false;
  bool allBackends = false;
  bool vulkanSmoke = false;
  bool noValidate = false;
  bool noBenchmark = false;
  bool showWindow = false;
  bool noShow = false;
  std::string vendorCountersMode = "single-pass";

  app.add_option("workload", workloadPath, "Path to workload YAML");
  app.add_option("--backend", backendName, "Force backend (vulkan|cuda|d3d12|...)");
  app.add_option("--format", format, "Output format: text|json")->check(CLI::IsMember({"text", "json"}));
  app.add_option("--log-level", logLevel, "error|warn|info|debug|trace")
      ->check(CLI::IsMember({"error", "warn", "info", "debug", "trace"}));
  app.add_flag("--list-backends", listBackends, "List registered backends");
  app.add_flag("--list-devices", listDevices, "List devices from available backends");
  app.add_flag("--inspect", inspectOnly, "Parse and report workload without GPU execute");
  app.add_flag("--no-validate", noValidate, "Skip CPU-reference validation");
  app.add_flag("--no-benchmark", noBenchmark, "Single timed run (skip warmup / multi-iter)");
  app.add_flag("--validate", validate, "Force validation on (default)");
  app.add_flag("--benchmark", benchmark, "Force benchmarking on (default)");
  app.add_flag("--all", allBackends, "Run across all available backends");
  app.add_flag("--show", showWindow, "Present graphics via Vulkan WSI (swapchain)");
  app.add_flag("--no-show", noShow, "Headless offscreen draw (no window)");
  app.add_option("--vendor-counters", vendorCountersMode,
                 "RADV HW counters: single-pass (default, safe on GFX12) | all (2-pass, may hang) | off")
      ->check(CLI::IsMember({"all", "single-pass", "off"}));

  app.add_option("--compile-spv", compileSpvIn,
                 "Compile handwritten GLSL (.comp/.vert/.frag/...) to SPIR-V");
  app.add_option("-o,--output", compileSpvOut, "Output .spv path for --compile-spv");
  app.add_option("--inspect-spv", inspectSpvPath, "Read SPIR-V header (+ spirv-dis head if available)");
  app.add_flag("--vulkan-smoke", vulkanSmoke,
               "Load SPIR-V on Vulkan and execute draw/dispatch (offscreen)");
  app.add_option("--spv", smokeComputeSpv, "Compute SPIR-V for --vulkan-smoke");
  app.add_option("--vert-spv", smokeVertSpv, "Vertex SPIR-V for --vulkan-smoke");
  app.add_option("--frag-spv", smokeFragSpv, "Fragment SPIR-V for --vulkan-smoke");
  auto *deviceIndexOpt = app.add_option(
      "--device-index", deviceIndex,
      "Physical device index (see --list-devices); omit to auto-pick best (prefers perf_query)");

  CLI11_PARSE(app, argc, argv);

  csrun::setLogLevel(parseLogLevel(logLevel));

  if (!inspectSpvPath.empty()) {
    auto info = csrun::SpirvToolchain::inspectSpvFile(inspectSpvPath);
    if (!info) {
      std::cerr << info.error().toString() << '\n';
      return 1;
    }
    std::cout << info.value().summary << '\n';
    auto dis = csrun::SpirvToolchain::disassembleSpvFile(inspectSpvPath, 40);
    if (dis && !dis.value().empty())
      std::cout << dis.value();
    return 0;
  }

  if (!compileSpvIn.empty()) {
    csrun::SpirvCompileRequest req;
    req.input = compileSpvIn;
    req.output = compileSpvOut;
    req.validate = true;
    req.keepOutput = true;
    auto result = csrun::SpirvToolchain::compile(req);
    if (!result) {
      std::cerr << result.error().toString() << '\n';
      return 1;
    }
    std::cout << "SPIR-V written: " << result.value().spvPath << '\n';
    std::cout << "stage: " << csrun::toString(result.value().stage)
              << "  words: " << result.value().words.size() << '\n';
    return 0;
  }

  if (vulkanSmoke) {
#if defined(CSRUN_HAS_VULKAN)
    csrun::VulkanSmokeRequest req;
    req.computeSpv = smokeComputeSpv;
    req.vertexSpv = smokeVertSpv;
    req.fragmentSpv = smokeFragSpv;
    if (deviceIndexOpt->count() > 0)
      req.deviceIndex = deviceIndex;
    auto smoke = csrun::vulkanSmokeSpirv(req);
    if (!smoke) {
      std::cerr << smoke.error().toString() << '\n';
      return 1;
    }
    const auto &smokeResult = smoke.value();
    // The verdict must reflect the evidence checks, not merely "it ran". A render that
    // failed validation is a failure even though the pipeline executed without error.
    std::cout << "Vulkan SPIR-V smoke: " << (smokeResult.ok ? "PASS" : "FAIL") << '\n';
    std::cout << "device: " << smokeResult.deviceName << '\n';
    std::cout << "mode:   " << smokeResult.mode << '\n';
    for (const auto &s : smokeResult.steps)
      std::cout << "  - " << s << '\n';
    if (!smokeResult.ok) {
      std::cerr << "smoke validation failed: rendered output did not pass evidence checks\n";
      return 1;
    }
    return 0;
#else
    std::cerr << "UNAVAILABLE: Vulkan backend not built\n";
    return 2;
#endif
  }

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
      std::cout << csrun::toString(d.backend) << '\t' << d.index << '\t' << d.name;
      if (!d.driver.empty())
        std::cout << "\t(" << d.driver << ")";
      std::cout << '\n';
    }
    return 0;
  }

  if (workloadPath.empty()) {
    std::cerr << "error: workload path required (or --compile-spv / --vulkan-smoke / "
                 "--list-backends)\n";
    return 1;
  }

  auto workload = runtime.loadWorkload(workloadPath);
  if (!workload) {
    std::cerr << workload.error().toString() << '\n';
    return 1;
  }

  if (allBackends) {
    csrun::ExecuteOptions opts;
    opts.validate = !noValidate;
    opts.benchmark = !noBenchmark;
    if (validate)
      opts.validate = true;
    if (benchmark)
      opts.benchmark = true;
    if (showWindow || noShow) {
      opts.showWindowSet = true;
      opts.showWindow = showWindow && !noShow;
    }
    if (deviceIndexOpt->count() > 0)
      opts.deviceIndex = deviceIndex;
    if (vendorCountersMode == "off")
      opts.vendorCounters = csrun::VendorCountersMode::Off;
    else if (vendorCountersMode == "single-pass")
      opts.vendorCounters = csrun::VendorCountersMode::PreferSinglePass;
    else
      opts.vendorCounters = csrun::VendorCountersMode::All;
    auto results = runtime.runAll(workload.value(), opts);
    if (!results) {
      std::cerr << results.error().toString() << '\n';
      return 1;
    }
    int failed = 0;
    for (const auto &r : results.value()) {
      if (format == "json") {
        auto json = csrun::JsonWriter::write(r);
        if (!json) {
          std::cerr << json.error().toString() << '\n';
          return 1;
        }
        std::cout << json.value() << '\n';
      } else {
        printTextResult(r);
        std::cout << "----\n";
      }
      if (r.validation == csrun::ValidationStatus::Failed)
        ++failed;
    }
    return failed ? 4 : 0;
  }

  csrun::ExecuteOptions opts;
  opts.validate = !noValidate;
  opts.benchmark = !noBenchmark;
  if (validate)
    opts.validate = true;
  if (benchmark)
    opts.benchmark = true;
  if (showWindow || noShow) {
    opts.showWindowSet = true;
    opts.showWindow = showWindow && !noShow;
  }
  if (deviceIndexOpt->count() > 0)
    opts.deviceIndex = deviceIndex;
  if (vendorCountersMode == "off")
    opts.vendorCounters = csrun::VendorCountersMode::Off;
  else if (vendorCountersMode == "single-pass")
    opts.vendorCounters = csrun::VendorCountersMode::PreferSinglePass;
  else
    opts.vendorCounters = csrun::VendorCountersMode::All;

  csrun::Expected<csrun::RunResult> result =
      inspectOnly ? runtime.inspect(workload.value(), forced)
                  : runtime.run(workload.value(), forced, opts);
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
