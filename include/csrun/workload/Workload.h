//===----------------------------------------------------------------------===//
// Workload.h - Backend-independent workload model
//===----------------------------------------------------------------------===//

#ifndef CSRUN_WORKLOAD_WORKLOAD_H
#define CSRUN_WORKLOAD_WORKLOAD_H

#include "csrun/backend/BackendType.h"
#include "csrun/core/Types.h"

#include <optional>
#include <string>
#include <vector>

namespace csrun {

struct ExecutionConfig {
  int warmup = 0;
  int iterations = 1;
};

struct ValidationConfig {
  bool enabled = true;
  double absoluteTolerance = 1e-5;
  double relativeTolerance = 1e-5;
};

struct Workload {
  std::string name;
  std::string operation;
  Dimensions dimensions;
  DataType inputDtype = DataType::FP32;
  DataType outputDtype = DataType::FP32;
  ExecutionConfig execution;
  ValidationConfig validation;
  std::optional<BackendType> preferredBackend;
  std::string sourcePath;
};

} // namespace csrun

#endif
