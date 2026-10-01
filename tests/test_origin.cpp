#include "core/Origin.h"

#include <catch2/catch_test_macros.hpp>

using namespace eluvian;

TEST_CASE("clients that are not browsers send no Origin and pass", "[origin]")
{
    CHECK(IsLocalOrigin(""));
}

TEST_CASE("local origins pass, with or without a port", "[origin]")
{
    CHECK(IsLocalOrigin("http://localhost"));
    CHECK(IsLocalOrigin("http://localhost:3000"));
    CHECK(IsLocalOrigin("https://127.0.0.1:8930"));
    CHECK(IsLocalOrigin("http://[::1]:8930"));
    CHECK(IsLocalOrigin("http://[::1]"));
}

TEST_CASE("anything else is refused", "[origin]")
{
    CHECK_FALSE(IsLocalOrigin("null"));
    CHECK_FALSE(IsLocalOrigin("https://example.com"));
    CHECK_FALSE(IsLocalOrigin("http://localhost.example.com"));
    CHECK_FALSE(IsLocalOrigin("http://127.0.0.1.nip.io"));
    CHECK_FALSE(IsLocalOrigin("http://localhost:"));
    CHECK_FALSE(IsLocalOrigin("http://localhost:80x"));
    CHECK_FALSE(IsLocalOrigin("http://[::1"));
    CHECK_FALSE(IsLocalOrigin("file://localhost"));
}
