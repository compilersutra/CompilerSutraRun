#include "csrun/backend/BackendSelector.h"

#include "csrun/core/Log.h"

#include <algorithm>
#include <vector>

namespace csrun {

BackendSelector::BackendSelector(const BackendRegistry &registry) : registry_(registry) {}

Expected<SelectionResult> BackendSelector::select(const Workload &workload,
                                                  std::optional<BackendType> forced) const {
  struct Candidate {
    Backend *backend = nullptr;
    DeviceInfo device;
    int priority = 0;
  };

  std::vector<Candidate> candidates;

  auto consider = [&](Backend &backend) {
    const auto info = backend.info();
    if (!info.available)
      return;
    auto devices = backend.enumerateDevices();
    if (!devices)
      return;
    for (const auto &d : devices.value()) {
      if (!d.available)
        continue;
      Candidate c;
      c.backend = &backend;
      c.device = d;
      c.priority = defaultPriority(info.type);
      candidates.push_back(c);
    }
  };

  if (forced) {
    Backend *b = registry_.find(*forced);
    if (!b) {
      Error e = Error::make(ErrorCode::BackendUnavailable,
                            std::string("requested backend not registered: ") +
                                std::string(toString(*forced)));
      e.backend = std::string(toString(*forced));
      return e;
    }
    const auto info = b->info();
    if (!info.available) {
      Error e = Error::make(ErrorCode::BackendUnavailable,
                            info.name + " backend unavailable: " + info.availabilityNote);
      e.backend = info.name;
      return e;
    }
    consider(*b);
  } else if (workload.preferredBackend) {
    if (Backend *b = registry_.find(*workload.preferredBackend))
      consider(*b);
  } else {
    for (const auto &b : registry_.backends())
      consider(*b);
  }

  if (candidates.empty()) {
    return Error::make(ErrorCode::BackendUnavailable,
                       "no available GPU backend can satisfy this workload yet "
                       "(Phase 1: backends are stubs; Vulkan lands in Phase 2)");
  }

  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate &a, const Candidate &b) { return a.priority < b.priority; });

  SelectionResult result;
  result.backend = candidates.front().backend->info().type;
  result.device = candidates.front().device;
  result.reason = "selected by stable priority among available backends";
  logInfo("backend", std::string("selected ") + std::string(toString(result.backend)));
  return result;
}

} // namespace csrun
