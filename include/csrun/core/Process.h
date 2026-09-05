//===----------------------------------------------------------------------===//
// Process.h - argv-based subprocess helper (no shell)
//===----------------------------------------------------------------------===//

#ifndef CSRUN_CORE_PROCESS_H
#define CSRUN_CORE_PROCESS_H

#include "csrun/core/Expected.h"

#include <string>
#include <vector>

namespace csrun {

struct ProcessResult {
  int exitCode = -1;
  std::string stdoutText;
  std::string stderrText;
};

/// Run executable with explicit argv. Does not invoke a shell.
Expected<ProcessResult> runProcess(const std::vector<std::string> &argv);

} // namespace csrun

#endif
