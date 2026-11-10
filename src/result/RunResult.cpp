#include "csrun/result/RunResult.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace csrun {
namespace {

std::string readFileLine(const std::filesystem::path &path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  return line;
}

std::string kernelVersion() {
  std::array<char, 256> buf{};
  if (std::system("uname -r > /tmp/csrun_kernel.txt 2>/dev/null") == 0)
    return readFileLine("/tmp/csrun_kernel.txt");
  return "unknown";
}

std::string cpuModel() {
  std::ifstream in("/proc/cpuinfo");
  std::string line;
  while (std::getline(in, line)) {
    if (line.find("model name") != std::string::npos) {
      auto pos = line.find(':');
      if (pos != std::string::npos)
        return line.substr(pos + 2);
    }
  }
  return "unknown";
}

std::string gpuDriverVersion() {
  for (const auto &entry : std::filesystem::directory_iterator("/sys/class/drm")) {
    if (!entry.is_directory())
      continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos)
      continue;
    auto uevent = readFileLine(entry.path() / "device" / "uevent");
    if (uevent.find("DRIVER=") == 0) {
      std::ifstream ver(entry.path() / "device" / "version");
      std::string v;
      std::getline(ver, v);
      return uevent.substr(7) + " " + v;
    }
  }
  return "unknown";
}

std::string gpuClocks() {
  for (const auto &entry : std::filesystem::directory_iterator("/sys/class/drm")) {
    if (!entry.is_directory())
      continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos)
      continue;
    auto hwmonDir = entry.path() / "device" / "hwmon";
    if (!std::filesystem::exists(hwmonDir))
      continue;
    for (const auto &h : std::filesystem::directory_iterator(hwmonDir)) {
      std::string clocks;
      for (uint32_t i = 1; i <= 2; ++i) {
        auto labelPath = h.path() / ("freq" + std::to_string(i) + "_label");
        auto inputPath = h.path() / ("freq" + std::to_string(i) + "_input");
        if (std::filesystem::exists(labelPath) && std::filesystem::exists(inputPath)) {
          auto label = readFileLine(labelPath);
          auto input = readFileLine(inputPath);
          if (!clocks.empty())
            clocks += ", ";
          clocks += label + "=" + std::to_string(std::stoull(input) / 1000000.0) + "MHz";
        }
      }
      if (!clocks.empty())
        return clocks;
    }
  }
  return "unknown";
}

} // namespace

EnvironmentInfo gatherEnvironmentInfo() {
  EnvironmentInfo info;
  info.kernelVersion = kernelVersion();
  info.cpuModel = cpuModel();
  info.gpuDriverVersion = gpuDriverVersion();
  info.gpuClocks = gpuClocks();
  info.csrunVersion = "0.1.0";
  return info;
}

std::string_view toString(ValidationStatus status) {
  switch (status) {
  case ValidationStatus::NotRun:
    return "not_run";
  case ValidationStatus::Passed:
    return "passed";
  case ValidationStatus::Failed:
    return "failed";
  case ValidationStatus::Skipped:
    return "skipped";
  }
  return "not_run";
}

Expected<std::string> JsonWriter::write(const RunResult &result) {
  nlohmann::json j;
  j["schema_version"] = result.schemaVersion;
  j["workload"] = {
      {"name", result.workloadName},
      {"operation", result.operation},
  };
  j["backend"] = {
      {"name", std::string(toString(result.backend))},
  };
  j["device"] = {
      {"vendor", result.deviceVendor},
      {"name", result.deviceName},
      {"driver", result.driver},
  };
  j["compiler"] = {
      {"name", result.compilerName},
  };
  j["execution"] = {
      {"warmup", result.warmup},
      {"iterations", result.iterations},
  };
  j["timing"] = {
      {"source", std::string(toString(result.timing.source))},
      {"min_ms", result.timing.minMs},
      {"median_ms", result.timing.medianMs},
      {"mean_ms", result.timing.meanMs},
      {"max_ms", result.timing.maxMs},
      {"stddev_ms", result.timing.stddevMs},
  };
  const auto &c = result.counters;
  const bool hasCounters = c.available || c.occlusionAvailable || c.gpuTimestampAvailable ||
                           c.hostRecordMs > 0.0 || c.hostSubmitMs > 0.0 ||
                           c.hostQueueWaitMs > 0.0 || !c.vendor.empty() || !c.vendorNote.empty();
  if (hasCounters) {
    nlohmann::json pipe = {
        {"ia_vertices", c.iaVertices},
        {"ia_primitives", c.iaPrimitives},
        {"vs_invocations", c.vsInvocations},
        {"tcs_patches", c.tcsPatches},
        {"tes_invocations", c.tesInvocations},
        {"gs_invocations", c.gsInvocations},
        {"gs_primitives", c.gsPrimitives},
        {"fs_invocations", c.fsInvocations},
        {"ps_invocations", c.fsInvocations},
        {"cs_invocations", c.csInvocations},
        {"clip_invocations", c.clipInvocations},
        {"clip_primitives", c.clipPrimitives},
        {"available", c.available},
    };
    nlohmann::json derived = {
        {"fs_per_vs", c.fsPerVs()},
        {"fs_per_primitive", c.fsPerPrimitive()},
        {"clip_reject_ratio", c.clipRejectRatio()},
        {"overdraw_estimate", c.overdrawEstimate()},
    };
    nlohmann::json host = {
        {"record_ms", c.hostRecordMs},
        {"submit_ms", c.hostSubmitMs},
        {"queue_wait_ms", c.hostQueueWaitMs},
        {"h2d_ms", c.hostH2dMs},
        {"d2h_ms", c.hostD2hMs},
    };
    nlohmann::json gpuTiming = {
        {"elapsed_ms", c.gpuElapsedMs},
        {"available", c.gpuTimestampAvailable},
    };
    nlohmann::json gpuStages = {
        {"available", c.gpuStages.available},
        {"kind", c.gpuStages.kind},
        {"total_ms", c.gpuStages.totalMs},
        {"top_to_vs_ms", c.gpuStages.topToVsMs},
        {"vs_to_fs_ms", c.gpuStages.vsToFsMs},
        {"fs_to_color_ms", c.gpuStages.fsToColorMs},
        {"color_to_bottom_ms", c.gpuStages.colorToBottomMs},
        {"top_to_cs_ms", c.gpuStages.topToCsMs},
        {"cs_to_bottom_ms", c.gpuStages.csToBottomMs},
    };
    nlohmann::json workload = {
        {"draw_count", c.drawCount},
        {"dispatch_count", c.dispatchCount},
    };
    nlohmann::json occlusion = {
        {"available", c.occlusionAvailable},
        {"samples_passed", c.occlusionSamplesPassed},
    };
    nlohmann::json vendor = {
        {"api", c.vendorApi},
        {"note", c.vendorNote},
        {"available", !c.vendor.empty()},
        {"counters", nlohmann::json::array()},
    };
    for (const auto &vc : c.vendor) {
      vendor["counters"].push_back({
          {"name", vc.name},
          {"category", vc.category},
          {"description", vc.description},
          {"unit", vc.unit},
          {"value", vc.value},
      });
    }
    j["counters"] = {
        {"pipeline_statistics", std::move(pipe)},
        {"derived", std::move(derived)},
        {"host", std::move(host)},
        {"gpu_timing", std::move(gpuTiming)},
        {"gpu_stages", std::move(gpuStages)},
        {"workload", std::move(workload)},
        {"occlusion", std::move(occlusion)},
        {"vendor", std::move(vendor)},
        // Flat aliases for older consumers / csperf bridge
        {"ia_vertices", c.iaVertices},
        {"ia_primitives", c.iaPrimitives},
        {"vs_invocations", c.vsInvocations},
        {"fs_invocations", c.fsInvocations},
        {"ps_invocations", c.fsInvocations},
        {"cs_invocations", c.csInvocations},
        {"clip_invocations", c.clipInvocations},
        {"clip_primitives", c.clipPrimitives},
        {"occlusion_samples_passed", c.occlusionSamplesPassed},
        {"gpu_elapsed_ms", c.gpuElapsedMs},
        {"host_record_ms", c.hostRecordMs},
        {"host_submit_ms", c.hostSubmitMs},
        {"host_queue_wait_ms", c.hostQueueWaitMs},
        {"host_h2d_ms", c.hostH2dMs},
        {"host_d2h_ms", c.hostD2hMs},
        {"fs_per_vs", c.fsPerVs()},
        {"fs_per_primitive", c.fsPerPrimitive()},
        {"overdraw_estimate", c.overdrawEstimate()},
    };
  }
  if (result.power.available) {
    j["power"] = {
        {"source", result.power.source},
        {"device", result.power.device},
        {"samples", result.power.samples},
        {"duration_ms", result.power.durationMs},
        {"avg_watts", result.power.avgWatts},
        {"min_watts", result.power.minWatts},
        {"max_watts", result.power.maxWatts},
        {"energy_joules", result.power.energyJoules},
        {"idle_watts", result.power.idleWatts},
        {"active_avg_watts", result.power.activeAvgWatts},
        {"energy_active_joules", result.power.energyActiveJoules},
    };
    if (!result.power.note.empty())
      j["power"]["note"] = result.power.note;
  }
  j["environment"] = {
      {"kernel", result.environment.kernelVersion},
      {"cpu", result.environment.cpuModel},
      {"gpu_driver", result.environment.gpuDriverVersion},
      {"gpu_clocks", result.environment.gpuClocks},
      {"csrun_version", result.environment.csrunVersion},
  };
  j["validation"] = {
      {"status", std::string(toString(result.validation))},
  };
  if (!result.note.empty())
    j["note"] = result.note;
  return j.dump(2);
}

} // namespace csrun
