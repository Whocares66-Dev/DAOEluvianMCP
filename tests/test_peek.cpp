#include "core/Peek.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstring>
#include <map>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// Memory of a few regions, each read whole or not at all; a class name for some addresses.
struct Fake
{
    std::map<std::uint32_t, std::vector<unsigned char>> regions;
    std::map<std::uint32_t, std::string> classes;

    template <class T> void Put(std::uint32_t address, const T &value)
    {
        for (auto &[start, bytes] : regions)
        {
            if (address >= start && address + sizeof(T) <= start + bytes.size())
            {
                std::memcpy(bytes.data() + (address - start), &value, sizeof(T));
                return;
            }
        }
        FAIL("no region holds the address");
    }

    PeekMemory Memory()
    {
        return {[this](std::uint32_t address, void *out, std::size_t size) {
                    return std::ranges::any_of(regions, [&](const auto &region) {
                        const auto &[start, bytes] = region;
                        if (address < start || address + size > start + bytes.size())
                        {
                            return false;
                        }
                        std::memcpy(out, bytes.data() + (address - start), size);
                        return true;
                    });
                },
                [this](std::uint32_t address) {
                    const auto found = classes.find(address);
                    return found != classes.end() ? found->second : std::string();
                }};
    }
};

// A creature at 0x1000 that holds at +0x7B0 an actor at 0x2000, whose height is at +0x64; and at
// +0x1F0 a pointer to nothing readable.
void Creature(Fake &fake)
{
    fake.regions[0x1000] = std::vector<unsigned char>(0x800);
    fake.regions[0x2000] = std::vector<unsigned char>(0x100);
    fake.classes[0x1000] = "CGameCreature";
    fake.classes[0x2000] = "CutsceneActor";
    fake.Put<std::uint32_t>(0x1000 + 0x7B0, 0x2000);
    fake.Put<std::uint32_t>(0x1000 + 0x1F0, 0xDEAD0000);
    fake.Put<float>(0x2000 + 0x64, 1.85F);
    fake.Put<float>(0x2000 + 0x10, 3.0F);
    fake.Put<float>(0x2000 + 0x14, -4.0F);
    fake.Put<float>(0x2000 + 0x18, 0.5F);
}

} // namespace

TEST_CASE("peek follows every offset but the last as a pointer and reads the value at the last", "[peek]")
{
    Fake fake;
    Creature(fake);
    const Json out = Peek(fake.Memory(), 0x1000, {0x7B0, 0x64}, "f32", 1);
    CHECK(out["base"] == "0x00001000");
    CHECK(out["class"] == "CGameCreature");
    REQUIRE(out["steps"].size() == 1);
    CHECK(out["steps"][0]["offset"] == "+0x7b0");
    CHECK(out["steps"][0]["at"] == "0x000017b0");
    CHECK(out["steps"][0]["pointer"] == "0x00002000");
    CHECK(out["steps"][0]["class"] == "CutsceneActor");
    CHECK(out["address"] == "0x00002064");
    CHECK(out["value"] == 1.85F);
    CHECK_FALSE(out.contains("readable"));
}

TEST_CASE("peek with one offset reads in the object itself, with none at its start", "[peek]")
{
    Fake fake;
    Creature(fake);
    CHECK(Peek(fake.Memory(), 0x1000, {0x7B0}, "u32", 1)["value"] == 0x2000);
    CHECK(Peek(fake.Memory(), 0x2010, {}, "vec3", 1)["value"] == Json::array({3.0, -4.0, 0.5}));
    CHECK(Peek(fake.Memory(), 0x2000, {0x14}, "f32", 2)["value"] == Json::array({-4.0, 0.5}));
    // A negative offset goes back from where the chain is.
    CHECK(Peek(fake.Memory(), 0x2074, {-0x10}, "f32", 1)["value"] == 1.85F);
}

TEST_CASE("peek's `ptr` names the class the pointer is to", "[peek]")
{
    Fake fake;
    Creature(fake);
    const Json out = Peek(fake.Memory(), 0x1000, {0x7B0}, "ptr", 1);
    CHECK(out["value"] == Json{{"pointer", "0x00002000"}, {"class", "CutsceneActor"}});
}

TEST_CASE("a read that fails ends the peek there, saying where", "[peek]")
{
    Fake fake;
    Creature(fake);
    const Json broken = Peek(fake.Memory(), 0x1000, {0x1F0, 0x64}, "f32", 1);
    CHECK(broken["readable"] == false);
    CHECK_THAT(broken["why"].get<std::string>(), ContainsSubstring("0xdead0064"));
    CHECK_FALSE(broken.contains("value"));

    const Json lost = Peek(fake.Memory(), 0x1000, {0x900, 0x64}, "f32", 1);
    CHECK(lost["readable"] == false);
    CHECK(lost["steps"].empty());
    CHECK_THAT(lost["why"].get<std::string>(), ContainsSubstring("pointer at 0x00001900 (+0x900)"));
}

TEST_CASE("peek reads the engine's wide string, inline while short and on the heap past 7", "[peek]")
{
    Fake fake;
    Creature(fake);
    // At 0x2020 a string of 6, inline; at 0x2040 one of 9 whose characters are at 0x2080.
    fake.Put<std::uint32_t>(0x2020 + 0x14, 6);
    fake.Put<std::uint32_t>(0x2020 + 0x18, 7);
    std::u16string shortText = u"duncan";
    std::memcpy(fake.regions[0x2000].data() + 0x24, shortText.data(), 12);
    fake.Put<std::uint32_t>(0x2040 + 0x04, 0x2080);
    fake.Put<std::uint32_t>(0x2040 + 0x14, 9);
    fake.Put<std::uint32_t>(0x2040 + 0x18, 15);
    std::u16string longText = u"greagoir!";
    std::memcpy(fake.regions[0x2000].data() + 0x80, longText.data(), 18);
    CHECK(Peek(fake.Memory(), 0x2020, {}, "wstring", 1)["value"] == "duncan");
    CHECK(Peek(fake.Memory(), 0x2040, {}, "wstring", 1)["value"] == "greagoir!");
    // A length past its capacity is no string.
    fake.Put<std::uint32_t>(0x2020 + 0x14, 50);
    CHECK(Peek(fake.Memory(), 0x2020, {}, "wstring", 1)["value"]["not_a_string"] == true);
}

TEST_CASE("peek's bytes are hex, its offsets numbers or hex text, and a type or count out of bounds refused", "[peek]")
{
    Fake fake;
    Creature(fake);
    CHECK(Peek(fake.Memory(), 0x2064, {}, "bytes", 4)["value"] == "cd cc ec 3f"); // 1.85F
    CHECK(PeekOffsets(Json::array({1968, "0x64", "-0x10", "16"})) == std::vector<std::int64_t>{1968, 100, -16, 16});
    CHECK(PeekOffsets(Json()).empty());
    CHECK_THROWS_WITH(PeekOffsets(Json::array({"0xZZ"})), ContainsSubstring("no number"));
    CHECK_THROWS_WITH(PeekOffsets(Json::array({"7B0"})), ContainsSubstring("no number"));
    CHECK_THROWS_WITH(PeekOffsets(Json("0x10")), ContainsSubstring("is a list"));
    CHECK_THROWS_WITH(Peek(fake.Memory(), 0x1000, {}, "double", 1), ContainsSubstring("none of"));
    CHECK_THROWS_WITH(Peek(fake.Memory(), 0x1000, {}, "u8", 0), ContainsSubstring("1 to 1024"));
}
