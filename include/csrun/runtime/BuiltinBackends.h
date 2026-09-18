//===----------------------------------------------------------------------===//
//
// BuiltinBackends.h - Single registration path for built-in backends
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_RUNTIME_BUILTINBACKENDS_H
#define CSRUN_RUNTIME_BUILTINBACKENDS_H

#include "csrun/backend/BackendRegistry.h"

namespace csrun {

/// Registers real backends when built, otherwise honest UNAVAILABLE stubs.
BackendRegistry createDefaultBackendRegistry();

} // namespace csrun

#endif
