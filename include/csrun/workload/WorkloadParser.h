//===----------------------------------------------------------------------===//
// WorkloadParser.h
//===----------------------------------------------------------------------===//

#ifndef CSRUN_WORKLOAD_WORKLOADPARSER_H
#define CSRUN_WORKLOAD_WORKLOADPARSER_H

#include "csrun/core/Expected.h"
#include "csrun/workload/Workload.h"

#include <filesystem>
#include <string_view>

namespace csrun {

class WorkloadParser {
public:
  /// Parse a YAML workload file from disk.
  static Expected<Workload> parseFile(const std::filesystem::path &path);

  /// Parse YAML text (primarily for tests).
  static Expected<Workload> parseString(std::string_view yamlText,
                                        std::string_view sourceName = "<memory>");
};

} // namespace csrun

#endif
