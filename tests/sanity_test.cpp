// sanity_test.cpp
//
// Purpose: verify that the CMake + GoogleTest + clang-format/clang-tidy infrastructure is wired
// up correctly *before* any real domain code is added to the project.
//
// This is intentionally the only test in the repository at this stage. Once it passes locally
// (via `ctest` and/or the IDE's test explorer), real domain code and its tests will be added
// alongside it - this file is not meant to be extended, only replaced/superseded later.

#include <gtest/gtest.h>
#include <string>

TEST(InfrastructureSanityCheck, BasicArithmeticWorks)
{
    EXPECT_EQ(2 + 2, 4);
}

TEST(InfrastructureSanityCheck, StringsCompareAsExpected)
{
    const std::string greeting{"MovieBackend"};

    EXPECT_EQ(greeting, "MovieBackend");
    EXPECT_NE(greeting, "SomethingElse");
}
