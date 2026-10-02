#include "core/Erf.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace eluvian;

namespace
{

// An archive's index as the game's files lay it out: the format's name in
// UTF-16, a count at 16, then each entry's name in 64 bytes of UTF-16.
std::vector<std::byte> Archive(const std::string &magic, std::size_t header, std::size_t entry,
                               const std::vector<std::string> &names)
{
    std::vector<std::byte> bytes(header + names.size() * entry);
    const auto put16 = [&bytes](std::size_t at, const std::string &text) {
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            bytes[at + 2 * i] = static_cast<std::byte>(text[i]);
        }
    };
    put16(0, magic);
    const auto count = static_cast<std::uint32_t>(names.size());
    for (std::size_t i = 0; i < 4; ++i)
    {
        bytes[16 + i] = static_cast<std::byte>(count >> (8 * i) & 0xFF);
    }
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        put16(header + i * entry, names[i]);
    }
    return bytes;
}

} // namespace

TEST_CASE("an ERF V2.0 index gives its names and its length", "[erf]")
{
    const auto bytes = Archive("ERF V2.0", 32, 72, {"ogre_default.utc", "hurlock_emissary_off.utc"});
    REQUIRE(ErfIndexBytes(bytes) == 32 + 2 * 72);
    CHECK(ErfNames(bytes) == std::vector<std::string>{"ogre_default.utc", "hurlock_emissary_off.utc"});
}

TEST_CASE("an ERF V2.2 index, the DLC's, has a longer header and entries", "[erf]")
{
    const auto bytes = Archive("ERF V2.2", 56, 76, {"a.uti", "b.plo", "c.utc"});
    REQUIRE(ErfIndexBytes(bytes) == 56 + 3 * 76);
    CHECK(ErfNames(bytes) == std::vector<std::string>{"a.uti", "b.plo", "c.utc"});
}

TEST_CASE("what is no ERF gives nothing, and an index cut short what it has", "[erf]")
{
    auto bytes = Archive("ERF V3.0", 32, 72, {"x.utc"});
    CHECK_FALSE(ErfIndexBytes(bytes));
    CHECK(ErfNames(bytes).empty());
    CHECK(ErfNames(std::vector<std::byte>(10)).empty());
    auto cut = Archive("ERF V2.0", 32, 72, {"one.utc", "two.utc"});
    cut.resize(32 + 72 + 10);
    CHECK(ErfNames(cut) == std::vector<std::string>{"one.utc"});
}
