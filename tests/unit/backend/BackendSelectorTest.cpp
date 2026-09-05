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

  auto sel = selector.select(w, csrun::BackendType::Vulkan);
  ASSERT_FALSE(sel);
  EXPECT_EQ(sel.error().code, csrun::ErrorCode::BackendUnavailable);
}

TEST(BackendRegistryTest, ListsHonestStubs) {
  auto reg = csrun::BackendRegistry::createDefault();
  ASSERT_GE(reg.backends().size(), 3u);
  for (const auto &b : reg.backends()) {
    EXPECT_FALSE(b->info().available);
  }
}
