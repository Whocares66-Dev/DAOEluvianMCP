#include "core/GfxList.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace eluvian;

TEST_CASE("a movie's list is read as its numbers, in order", "[gfxlist]")
{
    CHECK(ListNumbers("3,17,44") == std::vector<std::int64_t>{3, 17, 44});
    CHECK(ListNumbers("12") == std::vector<std::int64_t>{12});
    CHECK(ListNumbers("12.0,3") == std::vector<std::int64_t>{12, 3});
}

TEST_CASE("blanks are let be and what is no number is left out", "[gfxlist]")
{
    CHECK(ListNumbers(" 4, 5 ") == std::vector<std::int64_t>{4, 5});
    CHECK(ListNumbers("7,,8") == std::vector<std::int64_t>{7, 8});
    CHECK(ListNumbers("1,undefined,2") == std::vector<std::int64_t>{1, 2});
    CHECK(ListNumbers("9x,10") == std::vector<std::int64_t>{10});
}

TEST_CASE("an empty or null answer is no list", "[gfxlist]")
{
    CHECK(ListNumbers("").empty());
    CHECK(ListNumbers("null").empty());
    CHECK(ListNumbers("undefined").empty());
    CHECK(ListNumbers(",").empty());
}
