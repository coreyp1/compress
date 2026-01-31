/**
 * @file test_errors.cpp
 *
 * Unit tests for error helpers in the Ghoti.io Compress library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/compress/errors.h>
#include <gtest/gtest.h>

TEST(Errors, StatusToString_KnownCodes) {
  EXPECT_STREQ(gcomp_status_to_string(GCOMP_OK), "GCOMP_OK");
  EXPECT_STREQ(
      gcomp_status_to_string(GCOMP_ERR_INVALID_ARG), "GCOMP_ERR_INVALID_ARG");
  EXPECT_STREQ(gcomp_status_to_string(GCOMP_ERR_MEMORY), "GCOMP_ERR_MEMORY");
  EXPECT_STREQ(gcomp_status_to_string(GCOMP_ERR_LIMIT), "GCOMP_ERR_LIMIT");
  EXPECT_STREQ(gcomp_status_to_string(GCOMP_ERR_CORRUPT), "GCOMP_ERR_CORRUPT");
  EXPECT_STREQ(
      gcomp_status_to_string(GCOMP_ERR_UNSUPPORTED), "GCOMP_ERR_UNSUPPORTED");
  EXPECT_STREQ(
      gcomp_status_to_string(GCOMP_ERR_INTERNAL), "GCOMP_ERR_INTERNAL");
  EXPECT_STREQ(gcomp_status_to_string(GCOMP_ERR_IO), "GCOMP_ERR_IO");
}

TEST(Errors, StatusToString_UnknownCode) {
  EXPECT_STREQ(
      gcomp_status_to_string((gcomp_status_t)12345), "GCOMP_ERR_UNKNOWN");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
