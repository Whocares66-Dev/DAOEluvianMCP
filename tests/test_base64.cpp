#include "core/Base64.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace eluvian;

TEST_CASE("base64 encodes as RFC 4648's examples", "[base64]")
{
    CHECK(Base64("").empty());
    CHECK(Base64("f") == "Zg==");
    CHECK(Base64("fo") == "Zm8=");
    CHECK(Base64("foo") == "Zm9v");
    CHECK(Base64("foob") == "Zm9vYg==");
    CHECK(Base64("fooba") == "Zm9vYmE=");
    CHECK(Base64("foobar") == "Zm9vYmFy");
}

TEST_CASE("base64 keeps every byte, high ones too", "[base64]")
{
    const std::string jpegStart("\xFF\xD8\xFF\xE0", 4);
    CHECK(Base64(jpegStart) == "/9j/4A==");
    CHECK(Base64(std::string("\0\0\0", 3)) == "AAAA");
}
