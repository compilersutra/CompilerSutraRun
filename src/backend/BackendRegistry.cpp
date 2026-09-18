#include "csrun/backend/BackendRegistry.h"

namespace csrun {

void BackendRegistry::registerBackend(std::unique_ptr<Backend> backend) {
  backends_.push_back(std::move(backend));
}

Backend *BackendRegistry::find(BackendType type) const {
  for (const auto &b : backends_) {
    if (b->info().type == type)
      return b.get();
  }
  return nullptr;
}

} // namespace csrun
