//===----------------------------------------------------------------------===//
// Error.h - Structured error model for CompilerSutraRun
//===----------------------------------------------------------------------===//

#ifndef CSRUN_CORE_ERROR_H
#define CSRUN_CORE_ERROR_H

#include <string>
#include <string_view>

namespace csrun {

enum class ErrorCode {
  Ok = 0,
  InvalidWorkload,
  UnsupportedOperation,
  UnsupportedDtype,
  BackendUnavailable,
  DeviceNotFound,
  CompilationFailed,
  ResourceAllocationFailed,
  ExecutionFailed,
  ValidationFailed,
  BenchmarkFailed,
  ToolchainNotFound,
  DriverError,
  Unimplemented,
  IoError,
  Internal,
};

struct Error {
  ErrorCode code = ErrorCode::Internal;
  std::string message;
  std::string backend;
  std::string operation;
  std::string context;

  static Error make(ErrorCode code, std::string message);
  std::string toString() const;
};

std::string_view toString(ErrorCode code);

} // namespace csrun

#endif
