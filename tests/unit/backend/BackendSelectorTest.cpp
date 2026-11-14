#include <gtest/gtest.h>

#include "csrun/backend/BackendSelector.h"
#include "csrun/core/Types.h"
#include "csrun/runtime/BuiltinBackends.h"
#include "csrun/runtime/PlatformRuntime.h"
#include "csrun/workload/Workload.h"

#include <optional>

TEST(BackendSelectorTest, ForcedUnavailableBackendReturnsError) {
  auto reg = csrun::createDefaultBackendRegistry();
  csrun::BackendSelector selector(reg);
  csrun::Workload w;
  w.name = "t";
  w.operation = "vector_add";
  w.execution.iterations = 1;

  auto sel = selector.select(w, csrun::BackendType::CUDA);
  ASSERT_FALSE(sel);
  EXPECT_EQ(sel.error().code, csrun::ErrorCode::BackendUnavailable);
}

TEST(BackendRegistryTest, RegistersExpectedBackends) {
  auto reg = csrun::createDefaultBackendRegistry();
  ASSERT_GE(reg.backends().size(), 10u);
  ASSERT_NE(reg.find(csrun::BackendType::CUDA), nullptr);
  ASSERT_NE(reg.find(csrun::BackendType::OpenGL), nullptr);
  ASSERT_NE(reg.find(csrun::BackendType::OpenCL), nullptr);
  ASSERT_NE(reg.find(csrun::BackendType::HIP), nullptr);
  ASSERT_NE(reg.find(csrun::BackendType::CPU), nullptr);
  EXPECT_FALSE(reg.find(csrun::BackendType::CUDA)->info().available);
  EXPECT_FALSE(reg.find(csrun::BackendType::OpenCL)->info().name.empty());
  EXPECT_FALSE(reg.find(csrun::BackendType::HIP)->info().name.empty());
  EXPECT_FALSE(reg.find(csrun::BackendType::OpenGL)->info().name.empty());
}

TEST(BackendRegistryTest, StubsReportUnavailableNotSuccess) {
  auto reg = csrun::createDefaultBackendRegistry();
  for (auto type : {csrun::BackendType::CUDA, csrun::BackendType::D3D12, csrun::BackendType::Metal,
                    csrun::BackendType::WebGPU, csrun::BackendType::OpenGLES,
                    csrun::BackendType::CPU}) {
    auto *b = reg.find(type);
    ASSERT_NE(b, nullptr) << csrun::toString(type);
    const auto info = b->info();
    EXPECT_FALSE(info.available) << csrun::toString(type);
    EXPECT_FALSE(info.availabilityNote.empty()) << csrun::toString(type);

    csrun::DeviceInfo dummy;
    dummy.index = 0;
    csrun::Workload w;
    w.name = "t";
    w.operation = "vector_add";
    auto exec = b->execute(w, dummy, {});
    ASSERT_FALSE(exec) << csrun::toString(type);
    EXPECT_EQ(exec.error().code, csrun::ErrorCode::BackendUnavailable);
  }
}

TEST(PlatformRuntimeTest, InvalidDeviceIndexFailsClosed) {
  csrun::PlatformRuntime runtime;
  // Use an *available* backend. Forcing Vulkan on CI hosts where it is only a
  // stub yields BackendUnavailable before device-index is checked.
  auto backends = runtime.listBackends();
  std::optional<csrun::BackendType> available;
  for (const auto &b : backends) {
    if (b.available) {
      available = b.type;
      break;
    }
  }
  if (!available)
    GTEST_SKIP() << "no available backends on this host";

  csrun::Workload w;
  w.name = "vector_add";
  w.operation = "vector_add";
  w.dimensions.count = 16;
  w.inputDtype = csrun::DataType::FP32;
  w.outputDtype = csrun::DataType::FP32;
  w.execution.warmup = 0;
  w.execution.iterations = 1;
  w.validation.enabled = true;

  csrun::ExecuteOptions opts;
  opts.deviceIndex = 99999; // deliberately out of range
  opts.benchmark = false;
  auto result = runtime.run(w, *available, opts);
  ASSERT_FALSE(result) << "out-of-range device-index must not succeed";
  EXPECT_EQ(result.error().code, csrun::ErrorCode::DeviceNotFound)
      << result.error().toString();
}
