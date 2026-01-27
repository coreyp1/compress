#include <gtest/gtest.h>
#include <ghoti.io/compress/compress.h>

// Basic test to ensure the library can be linked
TEST(CompressLibrary, BasicTest) {
    EXPECT_TRUE(true);
}

// Test version functions
TEST(CompressLibrary, VersionTest) {
    EXPECT_EQ(gcomp_version_major(), GCOMP_VERSION_MAJOR);
    EXPECT_EQ(gcomp_version_minor(), GCOMP_VERSION_MINOR);
    EXPECT_EQ(gcomp_version_patch(), GCOMP_VERSION_PATCH);
    EXPECT_NE(gcomp_version_string(), nullptr);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
