//===----------------------------------------------------------------------===//
// RunResult.h / JsonWriter.h
//===----------------------------------------------------------------------===//

#ifndef CSRUN_RESULT_RUNRESULT_H
#define CSRUN_RESULT_RUNRESULT_H

#include "csrun/backend/BackendType.h"
#include "csrun/core/Expected.h"

#include <string>

namespace csrun {

enum class ValidationStatus { NotRun, Passed, Failed, Skipped };
enum class TimingSource { Unknown, CpuWall, GpuTimestamp };

struct TimingStats {
  TimingSource source = TimingSource::Unknown;
  double minMs = 0;
  double medianMs = 0;
  double meanMs = 0;
  double maxMs = 0;
  double stddevMs = 0;
};

struct RunResult {
  int schemaVersion = 1;
  std::string workloadName;
  std::string operation;
  BackendType backend{};
  std::string deviceName;
  std::string deviceVendor;
  std::string driver;
  std::string compilerName;
  int warmup = 0;
  int iterations = 0;
  TimingStats timing;
  ValidationStatus validation = ValidationStatus::NotRun;
  std::string note;
};

class JsonWriter {
public:
  static Expected<std::string> write(const RunResult &result);
};

std::string_view toString(ValidationStatus status);
std::string_view toString(TimingSource source);

} // namespace csrun

#endif
