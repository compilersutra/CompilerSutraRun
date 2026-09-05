#include <gtest/gtest.h>

#include "csrun/result/RunResult.h"

TEST(JsonWriterTest, EmitsSchemaVersion) {
  csrun::RunResult r;
  r.workloadName = "vector_add";
  r.operation = "vector_add";
  r.backend = csrun::BackendType::Vulkan;
  r.validation = csrun::ValidationStatus::Skipped;
  auto json = csrun::JsonWriter::write(r);
  ASSERT_TRUE(json);
  EXPECT_NE(json.value().find("\"schema_version\": 1"), std::string::npos);
  EXPECT_NE(json.value().find("vector_add"), std::string::npos);
}
