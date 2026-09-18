//===----------------------------------------------------------------------===//
//
// ExecuteOptions.h
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKEND_EXECUTEOPTIONS_H
#define CSRUN_BACKEND_EXECUTEOPTIONS_H

namespace csrun {

/// Control-plane knobs for Backend::execute (CLI / runtime).
struct ExecuteOptions {
  bool validate = true;
  bool benchmark = true;
};

} // namespace csrun

#endif
