#include <gtest/gtest.h>

#include "csrun/backend/BackendSelector.h"
#include "csrun/runtime/BuiltinBackends.h"
#include "csrun/workload/Workload.h"

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
