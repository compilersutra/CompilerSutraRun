#include <gtest/gtest.h>

#include "csrun/backend/BackendRegistry.h"
#include "csrun/backend/BackendSelector.h"
#include "csrun/workload/Workload.h"

TEST(BackendSelectorTest, ForcedUnavailableBackendReturnsError) {
  auto reg = csrun::BackendRegistry::createDefault();
  csrun::BackendSelector selector(reg);
  csrun::Workload w;
  w.name = "t";
  w.operation = "vector_add";
  w.execution.iterations = 1;

  // CUDA remains an honest stub in Phase 2.
  auto sel = selector.select(w, csrun::BackendType::CUDA);
  ASSERT_FALSE(sel);
  EXPECT_EQ(sel.error().code, csrun::ErrorCode::BackendUnavailable);
}

TEST(BackendRegistryTest, RegistersExpectedBackends) {
  auto reg = csrun::BackendRegistry::createDefault();
  ASSERT_GE(reg.backends().size(), 3u);
  ASSERT_NE(reg.find(csrun::BackendType::CUDA), nullptr);
  EXPECT_FALSE(reg.find(csrun::BackendType::CUDA)->info().available);
}
