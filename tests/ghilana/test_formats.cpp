// What ghilana reads its files with, over files made in the test: text,
// the PNG it writes, GFF3, GDA, the M2DA merge, the settings files.
#include "Build.h"

#include "ghilana/FileJson.h"
#include "ghilana/Folders.h"
#include "ghilana/Names.h"
#include "ghilana/Png.h"
#include "ghilana/Tables.h"
#include "ghilana/Text.h"

#include <dao/core/GdaFile.h>
#include <dao/core/GdaHash.h>
#include <dao/core/Gff3.h>
#include <dao/core/Gff4.h>

#include <catch2/catch_test_macros.hpp>
#include <spng.h>

#include <cstring>
#include <stdexcept>

using namespace eluvian;
using namespace eluvian::ghilana;
using namespace ghtest;

TEST_CASE("text: UTF-16 and Latin-1 to UTF-8 and back", "[ghilana][text]")
{
    CHECK(Utf8(L"caf\u00e9") == "caf\xC3\xA9");
    CHECK(Utf8(std::wstring{L'a', static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00)}) == "a\xF0\x9F\x98\x80");
    CHECK(Utf8(std::wstring{static_cast<wchar_t>(0xDC00)}) == "\xEF\xBF\xBD");
    CHECK(Wide("caf\xC3\xA9") == L"caf\u00e9");
    CHECK(Wide("caf\xE9") == L"caf\u00e9"); // not UTF-8: Latin-1
    CHECK(Latin1("caf\xE9") == "caf\xC3\xA9");
}

TEST_CASE("text: patterns without case, names, paths", "[ghilana][text]")
{
    CHECK(Glob("abi_*.gda", "ABI_base.gda"));
    CHECK(Glob("*ogre*", "creature_ogre_default.utc"));
    CHECK(Glob("a?c", "abc"));
    CHECK_FALSE(Glob("a?c", "ac"));
    CHECK_FALSE(Glob("abi_*.gda", "abi_base.gdax"));
    CHECK(Glob("*", ""));
    CHECK(Extension("Ogre.UTC") == "utc");
    CHECK(Extension("noext").empty());
    CHECK(Stem("abi_base.gda") == "abi_base");
    CHECK(PathKey(".\\Packages\\Core/Data\\2DA.erf") == "packages/core/data/2da.erf");
}

TEST_CASE("png: a picture of each channel count decodes back to its pixels", "[ghilana][png]")
{
    for (int channels = 1; channels <= 4; ++channels)
    {
        const std::uint32_t width = 37;
        const std::uint32_t height = 11;
        std::vector<std::uint8_t> pixels(std::size_t{width} * height * static_cast<std::size_t>(channels));
        for (std::size_t i = 0; i < pixels.size(); ++i)
        {
            pixels[i] = static_cast<std::uint8_t>((i * 7) % 251);
        }
        const auto png = EncodePng(width, height, channels, pixels);
        REQUIRE(png.size() > 8);
        CHECK(std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) == 0);

        spng_ctx *context = spng_ctx_new(0);
        REQUIRE(context != nullptr);
        REQUIRE(spng_set_png_buffer(context, png.data(), png.size()) == 0);
        spng_ihdr header{};
        REQUIRE(spng_get_ihdr(context, &header) == 0);
        CHECK(header.width == width);
        CHECK(header.height == height);
        CHECK(header.bit_depth == 8);
        std::vector<std::uint8_t> decoded(pixels.size());
        CHECK(spng_decode_image(context, decoded.data(), decoded.size(), SPNG_FMT_PNG, 0) == 0);
        spng_ctx_free(context);
        CHECK(decoded == pixels);
    }
    CHECK_THROWS_AS(EncodePng(2, 2, 4, std::vector<std::uint8_t>(15)), std::invalid_argument);
    CHECK_THROWS_AS(EncodePng(2, 2, 5, std::vector<std::uint8_t>(20)), std::invalid_argument);
}
TEST_CASE("json: a GFF3 template's fields by their labels, structs and lists nested, lists cut", "[ghilana][json]")
{
    Gff3 build("UTC");
    const auto tag = build.ExoString("caf\xE9");
    const auto name = build.LocString(100);
    const auto res = build.ResRef("gen_im_wep_axe");
    build.Struct(
        0xFFFFFFFF,
        {{"Appearance_Type", 4, 80}, {"Tag", 10, tag}, {"LocName", 12, name}, {"Items", 15, 0}, {"Missing", 10, 1000}});
    const auto item = build.Struct(0, {{"InventoryRes", 11, res}});
    build.List({item});
    const auto bytes = build.Done();
    const auto file = dao::core::gff3::File::Parse(bytes);
    REQUIRE(file);
    const Json json = Gff3ToJson(*file);
    CHECK(json["Appearance_Type"] == 80);
    CHECK(json["Tag"] == "caf\xC3\xA9"); // Latin-1 read as UTF-8
    CHECK(json["LocName"]["strref"] == 100);
    REQUIRE(json["Items"].size() == 1);
    CHECK(json["Items"][0]["InventoryRes"] == "gen_im_wep_axe");
    CHECK(json["Missing"].contains("_unread"));

    // A list longer than `items` keeps that many and says how many more.
    Gff3 many("UTI");
    many.Struct(0xFFFFFFFF, {{"L", 15, 0}});
    std::vector<std::uint32_t> structs;
    structs.reserve(5);
    for (int i = 0; i < 5; ++i)
    {
        structs.push_back(many.Struct(0, {{"N", 4, static_cast<std::uint32_t>(i)}}));
    }
    many.List(structs);
    const auto manyBytes = many.Done();
    const Json cut = Gff3ToJson(*dao::core::gff3::File::Parse(manyBytes), Limits{32, 2});
    REQUIRE(cut["L"].size() == 3);
    CHECK(cut["L"][2] == "... 3 more");
    // A struct deeper than `depth` is its field count.
    const Json shallow = Gff3ToJson(*dao::core::gff3::File::Parse(manyBytes), Limits{0, 10});
    CHECK(shallow["L"][0]["_fields"] == 1);
}

TEST_CASE("json: a table's cells and columns as answers give them", "[ghilana][json]")
{
    CHECK(CellJson(dao::core::GdaFile::Cell{}).is_null());
    CHECK(CellJson(dao::core::GdaFile::Cell{std::int64_t{-3}}) == -3);
    CHECK(CellJson(dao::core::GdaFile::Cell{2.5}) == 2.5);
    CHECK(CellJson(dao::core::GdaFile::Cell{std::wstring(L"caf\u00e9")}) == "caf\xC3\xA9");
    CHECK(ColumnName(dao::core::GdaColumnHash("TargetType")) == "TargetType");
    CHECK(ColumnName(0x12345678) == "#12345678");
    CHECK(ColumnType({1, dao::core::gff4::Type::F32}) == "f32");
    CHECK(ColumnType({1, std::nullopt}).empty());
}

TEST_CASE("tables: a call's value matches a cell by number or by text without case", "[ghilana][tables]")
{
    CHECK(CellMatches(Json(3), Json(3.0)));
    CHECK(CellMatches(Json("Flurry"), Json("flurry")));
    CHECK(CellMatches(Json(80), Json("80")));
    CHECK_FALSE(CellMatches(Json(nullptr), Json(0)));
    CHECK_FALSE(CellMatches(Json(1), Json(2)));
}

TEST_CASE("steam: libraryfolders.vdf's library paths, their backslashes unescaped", "[ghilana][steam]")
{
    const auto libraries =
        SteamLibraries("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files "
                       "(x86)\\\\Steam\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"E:\\\\SteamLibrary\"\n");
    REQUIRE(libraries.size() == 2);
    CHECK(libraries[0] == "C:\\Program Files (x86)\\Steam");
    CHECK(libraries[1] == "E:\\SteamLibrary");
}

TEST_CASE("names: GFF4 fields and GDA columns are named from DAOLib's tables", "[ghilana][names]")
{
    CHECK(Gff4FieldName(1) == "Tag");
    CHECK(Gff4FieldLabel("tag") == 1U);
    CHECK(Gff4FieldLabel("#19001") == 19001U);
    CHECK_FALSE(Gff4FieldLabel("no such field name"));
    CHECK(GdaColumnName(dao::core::GdaColumnHash("TargetType")) == "TargetType");
    CHECK(Gff4FieldNames() > 2000);
    CHECK(GdaColumnNames() > 1000);
}
