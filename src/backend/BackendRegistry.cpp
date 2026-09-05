#include "csrun/backend/BackendRegistry.h"

#include "csrun/core/Log.h"

namespace csrun {
namespace {

class StubBackend final : public Backend {
public:
  explicit StubBackend(BackendType type, std::string note)
      : type_(type), note_(std::move(note)) {}

  BackendInfo info() const override {
    BackendInfo i;
    i.type = type_;
    i.name = std::string(toString(type_));
    i.available = false;
    i.availabilityNote = note_;
    return i;
  }

  Expected<std::vector<DeviceInfo>> enumerateDevices() const override {
    return Error::make(ErrorCode::BackendUnavailable,
                       std::string(toString(type_)) + " backend unavailable: " + note_);
  }

private:
  BackendType type_;
  std::string note_;
};

} // namespace

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

BackendRegistry BackendRegistry::createDefault() {
  BackendRegistry reg;
  // Honest stubs: Phase 1 does not fake GPU availability.
  reg.registerBackend(std::make_unique<StubBackend>(
      BackendType::Vulkan, "Vulkan backend not built yet (enable in Phase 2)"));
  reg.registerBackend(std::make_unique<StubBackend>(
      BackendType::CUDA, "CUDA backend not built yet (enable in Phase 4)"));
  reg.registerBackend(std::make_unique<StubBackend>(
      BackendType::D3D12, "D3D12 backend not built yet (Windows / Phase 7)"));
  logInfo("backend", "default registry created with honest UNAVAILABLE stubs");
  return reg;
}

} // namespace csrun
