// The tools of a mod packed for the game (#140): mod_pack (an .erf, or a
// .dazip add-in for DAUpdater) and talk_table (a mod's own names).
#include "Fixture.h"

#include "ghilana/Text.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <dao/core/Erf.h>
#include <dao/core/TalkTable.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace eluvian;
using namespace ghtest;
using Catch::Matchers::ContainsSubstring;

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

std::string Text(const std::vector<std::byte> &bytes)
{
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
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

TEST_CASE("mod_pack: files as one archive the game reads, each by its name", "[ghilana][mods]")
{
    Served served;
    const auto in = served.install.root.path / L"in";
    served.install.root.Write(L"in/Red_D.dds", FromText("red"));
    served.install.root.Write(L"in/ogre.utc", FromText("utc"));
    const auto out = served.install.root.path / L"out";
    const Json answer =
        served.Call("mod_pack", {{"format", "erf"},
                                 {"files", {ghilana::PathText(in / L"Red_D.dds"), ghilana::PathText(in / L"ogre.utc")}},
                                 {"name", "mine"},
                                 {"to", ghilana::PathText(out)}});
    CHECK(answer["files"] == Json::array({"red_d.dds", "ogre.utc"}));
    const auto archive = dao::core::erf::Archive::Open(out / L"mine.erf");
    REQUIRE(archive);
    CHECK(archive->Find(L"red_d.dds") != nullptr);
    CHECK(archive->Find(L"ogre.utc") != nullptr);
}

TEST_CASE("mod_pack: a .dazip carries its manifest, the files in core/override and a talk table in talktables",
          "[ghilana][mods]")
{
    Served served;
    const auto in = served.install.root.path / L"in";
    served.install.root.Write(L"in/red_d.dds", FromText("red"));
    served.install.root.Write(L"in/mine_c_en-us.tlk", FromText("tlk"));
    const auto out = served.install.root.path / L"out";
    served.Call("mod_pack",
                {{"format", "dazip"},
                 {"files", {ghilana::PathText(in / L"red_d.dds"), ghilana::PathText(in / L"mine_c_en-us.tlk")}},
                 {"uid", "Mine"},
                 {"title", "Mine"},
                 {"to", ghilana::PathText(out)}});
    // A stored zip: its names and the manifest are there in plain bytes.
    const std::string zip = Text(FileBytes(out / L"Mine.dazip"));
    CHECK_THAT(zip, ContainsSubstring("Contents/addins/Mine/core/override/red_d.dds"));
    CHECK_THAT(zip, ContainsSubstring("Contents/addins/Mine/core/data/talktables/mine_c_en-us.tlk"));
    CHECK_THAT(zip, ContainsSubstring(R"(UID="Mine")"));
    CHECK_THAT(zip, ContainsSubstring("<Type>1</Type>")); // an entry the game enables (DAOLib #122)

    CHECK_THAT(Refusal(served, "mod_pack",
                       {{"format", "dazip"},
                        {"files", {ghilana::PathText(in / L"red_d.dds")}},
                        {"uid", "no spaces"},
                        {"to", ghilana::PathText(out)}}),
               ContainsSubstring("letters, digits and _"));
    CHECK_THAT(Refusal(served, "mod_pack",
                       {{"format", "erf"},
                        {"files", {ghilana::PathText(in / L"red_d.dds"), ghilana::PathText(in / L"RED_D.dds")}},
                        {"to", ghilana::PathText(out)}}),
               ContainsSubstring("two files are named red_d.dds"));
}

TEST_CASE("talk_table: a mod's own strings by id, named for its add-in", "[ghilana][mods]")
{
    Served served;
    const auto out = served.install.root.path / L"out";
    const Json answer = served.Call("talk_table", {{"strings", {{"99000001", "Red Ogre"}, {"99000002", "Red Axe"}}},
                                                   {"uid", "Mine"},
                                                   {"to", ghilana::PathText(out)}});
    CHECK(answer["strings"] == 2);
    const auto bytes = FileBytes(out / L"Mine_c_en-us.tlk");
    const auto table = dao::core::TalkTableFile::Parse(bytes);
    REQUIRE(table);
    CHECK(table->String(99000001) == L"Red Ogre");
    CHECK(table->String(99000002) == L"Red Axe");
    CHECK_THAT(Refusal(served, "talk_table",
                       {{"strings", {{"ogre", "Red Ogre"}}}, {"uid", "Mine"}, {"to", ghilana::PathText(out)}}),
               ContainsSubstring("no string reference"));
}
