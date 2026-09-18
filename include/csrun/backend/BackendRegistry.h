//===----------------------------------------------------------------------===//
// BackendRegistry.h
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKEND_BACKENDREGISTRY_H
#define CSRUN_BACKEND_BACKENDREGISTRY_H

#include "csrun/backend/Backend.h"

#include <memory>
#include <vector>

namespace csrun {

class BackendRegistry {
public:
  void registerBackend(std::unique_ptr<Backend> backend);
  Backend *find(BackendType type) const;
  const std::vector<std::unique_ptr<Backend>> &backends() const { return backends_; }

private:
  std::vector<std::unique_ptr<Backend>> backends_;
};

} // namespace csrun

#endif
