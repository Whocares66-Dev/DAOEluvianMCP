// The tools of a texture's round trip through an image editor (#135):
// texture_export (a DDS as a PNG) and texture_import (a PNG as the game's
// DDS), over the fixture's ogre_d.dds, 8 by 8 of 32 bits a pixel, red over a
// green half-size picture.
#include "Fixture.h"

#include "ghilana/Png.h"
#include "ghilana/Text.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <dao/core/Dds.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace eluvian;
using namespace ghtest;
using Catch::Matchers::ContainsSubstring;
namespace dds = dao::core::dds;

namespace
{

std::vector<std::byte> FileBytes(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::byte> out(chars.size());
    std::memcpy(out.data(), chars.data(), chars.size());
    return out;
}

void WritePng(const std::filesystem::path &path, std::uint32_t side, std::array<std::uint8_t, 4> rgba)
{
    std::vector<std::uint8_t> pixels;
    for (std::uint32_t i = 0; i < side * side; ++i)
    {
        pixels.insert(pixels.end(), rgba.begin(), rgba.end());
    }
    const auto png = ghilana::EncodePng(side, side, 4, pixels);
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
}

std::string Refusal(Served &served, const char *tool, const Json &args)
{
    try
    {
        served.Call(tool, args);
    }
    catch (const std::exception &error)
    {
        return error.what();
    }
    return {};
}

} // namespace

TEST_CASE("texture_export: a DDS's largest picture as a PNG, red, green, blue and alpha", "[ghilana][textures]")
{
    Served served;
    const auto out = served.install.root.path / L"out";
    const Json answer =
        served.Call("texture_export", {{"name", "ogre_d"}, {"source", "core"}, {"to", ghilana::PathText(out)}});
    CHECK(answer["format"] == "B8G8R8A8");
    CHECK(answer["width"] == 8);
    CHECK(answer["normal_map"] == false);
    const auto png = ghilana::DecodePng(FileBytes(out / L"ogre_d.png"));
    REQUIRE(png.width == 8);
    REQUIRE(png.height == 8);
    CHECK(png.rgba[0] == 255);
    CHECK(png.rgba[1] == 0);
    CHECK(png.rgba[3] == 255);
}

TEST_CASE("texture_import: a PNG as the game's DDS, in the original's format with its every halving",
          "[ghilana][textures]")
{
    Served served;
    const auto out = served.install.root.path / L"out";
    std::filesystem::create_directories(out);
    WritePng(out / L"blue.png", 16, {0, 0, 255, 255});
    const Json answer = served.Call("texture_import", {{"name", "ogre_d"},
                                                       {"source", "core"},
                                                       {"png", ghilana::PathText(out / L"blue.png")},
                                                       {"to", ghilana::PathText(out / L"back")}});
    CHECK(answer["format"] == "A8R8G8B8"); // the original is 32 bits a pixel
    CHECK(answer["width"] == 16);
    const auto bytes = FileBytes(out / L"back" / L"ogre_d.dds");
    const auto described = dds::Describe(bytes);
    REQUIRE(described);
    CHECK(described->width == 16);
    CHECK(described->pictures == 5); // 16, 8, 4, 2, 1
    const auto picture = dds::Decode(bytes);
    REQUIRE(picture);
    CHECK(picture->rgba[0] == 0);
    CHECK(picture->rgba[2] == 255);

    // Another format when asked.
    served.Call("texture_import", {{"name", "ogre_d"},
                                   {"source", "core"},
                                   {"png", ghilana::PathText(out / L"blue.png")},
                                   {"format", "dxt1"},
                                   {"to", ghilana::PathText(out / L"dxt1")}});
    const auto dxt1 = dds::Describe(FileBytes(out / L"dxt1" / L"ogre_d.dds"));
    REQUIRE(dxt1);
    CHECK(dxt1->format == "DXT1");
}

TEST_CASE("texture_import: refuses a picture whose sides are not powers of two, and a file that is no PNG",
          "[ghilana][textures]")
{
    Served served;
    const auto out = served.install.root.path / L"out";
    std::filesystem::create_directories(out);
    WritePng(out / L"odd.png", 6, {0, 0, 255, 255});
    CHECK_THAT(Refusal(served, "texture_import",
                       {{"name", "ogre_d"},
                        {"source", "core"},
                        {"png", ghilana::PathText(out / L"odd.png")},
                        {"to", ghilana::PathText(out / L"x")}}),
               ContainsSubstring("powers of two"));
    std::ofstream(out / L"not.png") << "no picture";
    CHECK_THAT(Refusal(served, "texture_import",
                       {{"name", "ogre_d"},
                        {"source", "core"},
                        {"png", ghilana::PathText(out / L"not.png")},
                        {"to", ghilana::PathText(out / L"x")}}),
               ContainsSubstring("DecodePng"));
}

TEST_CASE("a normal map goes out as x, y, z and comes back in the game's layout, x in alpha", "[ghilana][textures]")
{
    Served served;
    const auto out = served.install.root.path / L"out";
    std::filesystem::create_directories(out);
    // Straight out of the surface, (0, 0, 1): 128, 128, 255.
    WritePng(out / L"flat.png", 8, {128, 128, 255, 255});
    const Json answer = served.Call("texture_import", {{"name", "ogre_d"},
                                                       {"source", "core"},
                                                       {"normal", true},
                                                       {"png", ghilana::PathText(out / L"flat.png")},
                                                       {"to", ghilana::PathText(out / L"n")}});
    CHECK(answer["normal_map"] == true);
    CHECK(answer["format"] == "DXT5, a normal map");
    const auto picture = dds::Decode(FileBytes(out / L"n" / L"ogre_d.dds"));
    REQUIRE(picture);
    // x in alpha and y in green, as the game's are.
    CHECK(std::abs(int{picture->rgba[3]} - 128) <= 2);
    CHECK(std::abs(int{picture->rgba[1]} - 128) <= 2);
    // And out again as x, y, z.
    const auto back = dds::Normals(*picture);
    CHECK(back.rgba[2] > 250);
}
