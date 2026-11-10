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
  EXPECT_NE(r.note.find("skipped"), std::string::npos);
}

TEST(VectorAddScaffoldTest, WorkloadValidationDisabledIsSkippedNotPassed) {
  csrun::Workload w;
  w.name = "t";
  w.operation = "vector_add";
  w.execution.iterations = 1;
  w.validation.enabled = false;

  csrun::DeviceInfo d;
  d.name = "dev";
  double samples[] = {1.0};
  float ref[] = {1.f};
  float act[] = {1.f};
  csrun::ExecuteOptions opts;
  opts.validate = true; // CLI wants validate, but workload disabled it
  auto r = csrun::fillVectorAddResult(w, d, csrun::BackendType::Vulkan, "glslang", opts, samples,
                                      csrun::TimingSource::GpuTimestamp, ref, act);
  EXPECT_EQ(r.validation, csrun::ValidationStatus::Skipped);
  EXPECT_NE(r.validation, csrun::ValidationStatus::Passed);
}

TEST(VectorAddScaffoldTest, MatchingOutputPasses) {
  csrun::Workload w;
  w.name = "t";
  w.operation = "vector_add";
  w.execution.iterations = 1;
  w.validation.enabled = true;
  w.validation.absoluteTolerance = 1e-5;
  w.validation.relativeTolerance = 1e-5;

  csrun::DeviceInfo d;
  d.name = "dev";
  double samples[] = {0.1};
  float ref[] = {1.f, 2.f, 3.f};
  float act[] = {1.f, 2.f, 3.f};
  csrun::ExecuteOptions opts;
  auto r = csrun::fillVectorAddResult(w, d, csrun::BackendType::HIP, "hiprtc", opts, samples,
                                      csrun::TimingSource::GpuTimestamp, ref, act);
  EXPECT_EQ(r.validation, csrun::ValidationStatus::Passed);
  EXPECT_EQ(r.timing.source, csrun::TimingSource::GpuTimestamp);
}

TEST(VectorAddScaffoldTest, KnownBadOutputFailsValidation) {
  csrun::Workload w;
  w.name = "t";
  w.operation = "vector_add";
  w.execution.iterations = 1;
  w.validation.enabled = true;
  w.validation.absoluteTolerance = 1e-5;
  w.validation.relativeTolerance = 1e-5;

  csrun::DeviceInfo d;
  d.name = "dev";
  double samples[] = {0.1};
  float ref[] = {1.f, 2.f, 3.f};
  float act[] = {1.f, 2.f, 3.5f}; // known-bad
  csrun::ExecuteOptions opts;
  auto r = csrun::fillVectorAddResult(w, d, csrun::BackendType::OpenCL, "opencl", opts, samples,
                                      csrun::TimingSource::GpuTimestamp, ref, act);
  EXPECT_EQ(r.validation, csrun::ValidationStatus::Failed);
  EXPECT_NE(r.note.find("FAIL"), std::string::npos);
}
