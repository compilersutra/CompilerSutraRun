#include <gtest/gtest.h>

#include "csrun/result/RunResult.h"

#include <string>

namespace {

csrun::RunResult baseResult() {
  csrun::RunResult r;
  r.schemaVersion = 3;
  r.workloadName = "vector_add";
  r.operation = "vector_add";
  r.backend = csrun::BackendType::Vulkan;
  r.deviceName = "AMD Radeon RX 9060 XT";
  r.deviceVendor = "AMD";
  r.driver = "Mesa radv";
  r.compilerName = "glslangValidator";
  r.warmup = 10;
  r.iterations = 100;
  r.timing.source = csrun::TimingSource::GpuTimestamp;
  r.timing.medianMs = 0.431;
  r.timing.meanMs = 0.44;
  r.timing.minMs = 0.4;
  r.timing.maxMs = 0.5;
  r.timing.stddevMs = 0.02;
  return r;
}

} // namespace

TEST(JsonWriterTest, EmitsSchemaVersion) {
  auto r = baseResult();
  r.validation = csrun::ValidationStatus::Skipped;
  auto json = csrun::JsonWriter::write(r);
  ASSERT_TRUE(json);
  EXPECT_NE(json.value().find("\"schema_version\": 3"), std::string::npos);
  EXPECT_NE(json.value().find("vector_add"), std::string::npos);
}

TEST(JsonWriterTest, EmitsPassedStatusAndTimingSource) {
  auto r = baseResult();
  r.validation = csrun::ValidationStatus::Passed;
  auto json = csrun::JsonWriter::write(r);
  ASSERT_TRUE(json);
  const std::string &s = json.value();
  EXPECT_NE(s.find("\"status\": \"passed\""), std::string::npos);
  EXPECT_NE(s.find("\"source\": \"gpu_timestamp\""), std::string::npos);
  EXPECT_NE(s.find("\"median_ms\""), std::string::npos);
  EXPECT_NE(s.find("AMD Radeon RX 9060 XT"), std::string::npos);
}

TEST(JsonWriterTest, EmitsFailedStatusDistinctFromSkipped) {
  auto failed = baseResult();
  failed.validation = csrun::ValidationStatus::Failed;
  auto skipped = baseResult();
  skipped.validation = csrun::ValidationStatus::Skipped;

  auto jf = csrun::JsonWriter::write(failed);
  auto js = csrun::JsonWriter::write(skipped);
  ASSERT_TRUE(jf);
  ASSERT_TRUE(js);
  EXPECT_NE(jf.value().find("\"status\": \"failed\""), std::string::npos);
  EXPECT_NE(js.value().find("\"status\": \"skipped\""), std::string::npos);
  EXPECT_EQ(jf.value().find("\"status\": \"passed\""), std::string::npos);
  EXPECT_EQ(js.value().find("\"status\": \"passed\""), std::string::npos);
}

TEST(JsonWriterTest, EmitsCpuWallSource) {
  auto r = baseResult();
  r.validation = csrun::ValidationStatus::Passed;
  r.timing.source = csrun::TimingSource::CpuWall;
  auto json = csrun::JsonWriter::write(r);
  ASSERT_TRUE(json);
  EXPECT_NE(json.value().find("\"source\": \"cpu_wall\""), std::string::npos);
}
