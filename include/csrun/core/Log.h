//===----------------------------------------------------------------------===//
// Log.h - Structured logging with subsystem tags
//===----------------------------------------------------------------------===//

#ifndef CSRUN_CORE_LOG_H
#define CSRUN_CORE_LOG_H

#include <string_view>

namespace csrun {

enum class LogLevel { Error, Warn, Info, Debug, Trace };

void setLogLevel(LogLevel level);
LogLevel getLogLevel();

void log(LogLevel level, std::string_view subsystem, std::string_view message);

inline void logError(std::string_view subsystem, std::string_view message) {
  log(LogLevel::Error, subsystem, message);
}
inline void logWarn(std::string_view subsystem, std::string_view message) {
  log(LogLevel::Warn, subsystem, message);
}
inline void logInfo(std::string_view subsystem, std::string_view message) {
  log(LogLevel::Info, subsystem, message);
}
inline void logDebug(std::string_view subsystem, std::string_view message) {
  log(LogLevel::Debug, subsystem, message);
}

} // namespace csrun

#endif
