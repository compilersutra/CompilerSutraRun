#include "csrun/core/Log.h"

#include <iostream>
#include <mutex>

namespace csrun {
namespace {

LogLevel gLevel = LogLevel::Info;
std::mutex gMutex;

const char *levelName(LogLevel level) {
  switch (level) {
  case LogLevel::Error:
    return "ERROR";
  case LogLevel::Warn:
    return "WARN";
  case LogLevel::Info:
    return "INFO";
  case LogLevel::Debug:
    return "DEBUG";
  case LogLevel::Trace:
    return "TRACE";
  }
  return "INFO";
}

int rank(LogLevel level) { return static_cast<int>(level); }

} // namespace

void setLogLevel(LogLevel level) {
  std::lock_guard lock(gMutex);
  gLevel = level;
}

LogLevel getLogLevel() {
  std::lock_guard lock(gMutex);
  return gLevel;
}

void log(LogLevel level, std::string_view subsystem, std::string_view message) {
  std::lock_guard lock(gMutex);
  if (rank(level) > rank(gLevel))
    return;
  std::cerr << '[' << levelName(level) << "][" << subsystem << "] " << message << '\n';
}

} // namespace csrun
