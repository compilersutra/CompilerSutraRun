#include "csrun/result/RunResult.h"

#include <nlohmann/json.hpp>

namespace csrun {

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
  j["validation"] = {
      {"status", std::string(toString(result.validation))},
  };
  if (!result.note.empty())
    j["note"] = result.note;
  return j.dump(2);
}

} // namespace csrun
