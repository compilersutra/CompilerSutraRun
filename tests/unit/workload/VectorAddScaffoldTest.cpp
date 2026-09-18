#include <gtest/gtest.h>

#include "csrun/workload/VectorAddScaffold.h"

TEST(VectorAddScaffoldTest, RequireRejectsNonVectorAdd) {
  csrun::Workload w;
  w.operation = "matmul";
  w.dimensions.count = 16;
  w.inputDtype = csrun::DataType::FP32;
  w.outputDtype = csrun::DataType::FP32;
  auto n = csrun::requireVectorAddFp32(w, "test");
  ASSERT_FALSE(n);
  EXPECT_EQ(n.error().code, csrun::ErrorCode::UnsupportedOperation);
}

TEST(VectorAddScaffoldTest, MakeHostMatchesReference) {
  auto host = csrun::makeVectorAddHost(8);
  ASSERT_EQ(host.count, 8u);
  EXPECT_FLOAT_EQ(host.reference[3], host.a[3] + host.b[3]);
}

TEST(VectorAddScaffoldTest, FillResultCanSkipValidation) {
  csrun::Workload w;
  w.name = "t";
  w.operation = "vector_add";
  w.execution.iterations = 2;
  w.validation.enabled = true;

  csrun::DeviceInfo d;
  d.name = "dev";
  double samples[] = {1.0, 2.0};
  float ref[] = {1.f, 2.f};
  float act[] = {1.f, 2.f};
  csrun::ExecuteOptions opts;
  opts.validate = false;
  auto r = csrun::fillVectorAddResult(w, d, csrun::BackendType::CPU, "none", opts, samples,
                                      csrun::TimingSource::CpuWall, ref, act);
  EXPECT_EQ(r.validation, csrun::ValidationStatus::Skipped);
}
