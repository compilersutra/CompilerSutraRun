#include <gtest/gtest.h>

#include "csrun/workload/WorkloadParser.h"

TEST(WorkloadParserTest, ParsesVectorAddYaml) {
  constexpr const char *kYaml = R"(
name: vector_add
operation: vector_add
dimensions:
  count: 1024
dtype:
  input: fp32
  output: fp32
execution:
  warmup: 2
  iterations: 5
validation:
  enabled: true
  tolerance: 1.0e-5
)";

  auto w = csrun::WorkloadParser::parseString(kYaml);
  ASSERT_TRUE(w) << w.error().toString();
  EXPECT_EQ(w.value().name, "vector_add");
  EXPECT_EQ(w.value().operation, "vector_add");
  EXPECT_EQ(w.value().dimensions.count, 1024u);
  EXPECT_EQ(w.value().inputDtype, csrun::DataType::FP32);
  EXPECT_EQ(w.value().execution.iterations, 5);
  EXPECT_TRUE(w.value().validation.enabled);
}

TEST(WorkloadParserTest, RejectsMissingName) {
  auto w = csrun::WorkloadParser::parseString("operation: matmul\n");
  ASSERT_FALSE(w);
  EXPECT_EQ(w.error().code, csrun::ErrorCode::InvalidWorkload);
}

TEST(WorkloadParserTest, ParsesHandwrittenSpirvShaders) {
  constexpr const char *kYaml = R"(
name: triangle
operation: spirv
backend: vulkan
shaders:
  - triangle.vert
  - path: triangle.frag
    stage: frag
)";
  auto w = csrun::WorkloadParser::parseString(kYaml);
  ASSERT_TRUE(w) << w.error().toString();
  ASSERT_EQ(w.value().shaders.size(), 2u);
  EXPECT_EQ(w.value().shaders[0].path, "triangle.vert");
  EXPECT_EQ(w.value().shaders[1].stage, "frag");
}

TEST(WorkloadParserTest, ParsesShaderMapForm) {
  constexpr const char *kYaml = R"(
name: t
operation: shader
shaders:
  vertex: a.vert
  fragment: b.frag
)";
  auto w = csrun::WorkloadParser::parseString(kYaml);
  ASSERT_TRUE(w) << w.error().toString();
  ASSERT_EQ(w.value().shaders.size(), 2u);
}
