#include "csrun/core/Error.h"

#include <sstream>

namespace csrun {

Error Error::make(ErrorCode code, std::string message) {
  Error e;
  e.code = code;
  e.message = std::move(message);
  return e;
}

std::string_view toString(ErrorCode code) {
  switch (code) {
  case ErrorCode::Ok:
    return "OK";
  case ErrorCode::InvalidWorkload:
    return "INVALID_WORKLOAD";
  case ErrorCode::UnsupportedOperation:
    return "UNSUPPORTED_OPERATION";
  case ErrorCode::UnsupportedDtype:
    return "UNSUPPORTED_DTYPE";
  case ErrorCode::BackendUnavailable:
    return "BACKEND_UNAVAILABLE";
  case ErrorCode::DeviceNotFound:
    return "DEVICE_NOT_FOUND";
  case ErrorCode::CompilationFailed:
    return "COMPILATION_FAILED";
  case ErrorCode::ResourceAllocationFailed:
    return "RESOURCE_ALLOCATION_FAILED";
  case ErrorCode::ExecutionFailed:
    return "EXECUTION_FAILED";
  case ErrorCode::ValidationFailed:
    return "VALIDATION_FAILED";
  case ErrorCode::BenchmarkFailed:
    return "BENCHMARK_FAILED";
  case ErrorCode::ToolchainNotFound:
    return "TOOLCHAIN_NOT_FOUND";
  case ErrorCode::DriverError:
    return "DRIVER_ERROR";
  case ErrorCode::Unimplemented:
    return "UNIMPLEMENTED";
  case ErrorCode::IoError:
    return "IO_ERROR";
  case ErrorCode::Internal:
    return "INTERNAL";
  }
  return "INTERNAL";
}

std::string Error::toString() const {
  std::ostringstream oss;
  oss << csrun::toString(code) << ": " << message;
  if (!backend.empty())
    oss << " [backend=" << backend << "]";
  if (!operation.empty())
    oss << " [operation=" << operation << "]";
  if (!context.empty())
    oss << " [context=" << context << "]";
  return oss.str();
}

} // namespace csrun
