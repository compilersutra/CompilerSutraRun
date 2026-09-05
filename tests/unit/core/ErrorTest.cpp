#include <gtest/gtest.h>

#include "csrun/core/Error.h"

TEST(ErrorTest, ToStringContainsCodeAndMessage) {
  auto e = csrun::Error::make(csrun::ErrorCode::BackendUnavailable, "no driver");
  e.backend = "cuda";
  const auto s = e.toString();
  EXPECT_NE(s.find("BACKEND_UNAVAILABLE"), std::string::npos);
  EXPECT_NE(s.find("no driver"), std::string::npos);
  EXPECT_NE(s.find("cuda"), std::string::npos);
}
