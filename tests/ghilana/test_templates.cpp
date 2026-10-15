// The tools of a creature or item of a mod's own (#141): table_rows over the
// fixture's abi table (abi_base's rows 1 and 2; 3 and 7 taken by a plugin's
// and an override's abi_*), template_edit over its ogre.utc, model_copy
// over DAOLib's slab model.
#include "Fixture.h"
#include "model_builder.h"

#include "ghilana/Text.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <dao/core/GdaFile.h>
#include <dao/core/Gff3.h>
#include <dao/core/Gff4.h>
#include <dao/core/Model.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <variant>

using namespace eluvian;
using namespace ghtest;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
namespace model = dao::core::model;

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

TEST_CASE("table_rows: a row the game has copied under a new ID, its cells changed", "[ghilana][templates]")
{
    Served served;
    const auto out = served.install.root.path / L"out";
    const Json answer = served.Call("table_rows", {{"table", "abi_base"},
                                                   {"rows", {{{"copy", 2}, {"set", {{"ID", 9}, {"Label", "Mine"}}}}}},
                                                   {"name", "abi_mine"},
                                                   {"to", ghilana::PathText(out)}});
    CHECK(answer["rows"] == Json::array({9}));
    CHECK(answer["merged_into"] == "abi");
    const auto bytes = FileBytes(out / L"abi_mine.gda");
    const auto table = dao::core::GdaFile::Parse(bytes);
    REQUIRE(table);
    REQUIRE(table->Rows().size() == 1);
    const auto &row = table->Rows()[0];
    CHECK(std::get<std::int64_t>(row[*table->ColumnOf("ID")]) == 9);
    CHECK(std::get<std::wstring>(row[*table->ColumnOf("Label")]) == L"Mine");
    CHECK(std::get<double>(row[*table->ColumnOf("Cost")]) == Approx(3)); // row 2's, kept
}

TEST_CASE("table_rows: refuses an ID the merged table has, a column it has not, and a name it does not merge",
          "[ghilana][templates]")
{
    Served served;
    const std::string to = ghilana::PathText(served.install.root.path / L"out");
    const auto rows = [](int id) { return Json::array({{{"copy", 1}, {"set", {{"ID", id}}}}}); };
    // 3 is a plugin's abi_mymod row, 7 an override's: the merged table has both.
    for (const int taken : {2, 3, 7})
    {
        CHECK_THAT(Refusal(served, "table_rows",
                           {{"table", "abi_base"}, {"rows", rows(taken)}, {"name", "abi_mine"}, {"to", to}}),
                   ContainsSubstring("is taken"));
    }
    CHECK_THAT(Refusal(served, "table_rows",
                       {{"table", "abi_base"},
                        {"rows", {{{"copy", 1}, {"set", {{"ID", 9}, {"Nope", 1}}}}}},
                        {"name", "abi_mine"},
                        {"to", to}}),
               ContainsSubstring("no column Nope"));
    CHECK_THAT(Refusal(served, "table_rows",
                       {{"table", "abi_base"},
                        {"rows", {{{"copy", 1}, {"set", {{"ID", 9}, {"Label", 5}}}}}},
                        {"name", "abi_mine"},
                        {"to", to}}),
               ContainsSubstring("holds text"));
    CHECK_THAT(Refusal(served, "table_rows", {{"table", "abi_base"}, {"rows", rows(9)}, {"name", "mine"}, {"to", to}}),
               ContainsSubstring("abi_<yours>"));
}

TEST_CASE("template_edit: a template under a new name with fields changed, by path", "[ghilana][templates]")
{
    Served served;
    const auto out = served.install.root.path / L"out";
    const Json answer =
        served.Call("template_edit", {{"name", "ogre.utc"},
                                      {"set", {{"Tag", "red_ogre"}, {"Appearance Data/Appearance_Type", 7701}}},
                                      {"save_as", "red_ogre"},
                                      {"to", ghilana::PathText(out)}});
    CHECK(answer["changed"].size() == 2);
    const auto bytes = FileBytes(out / L"red_ogre.utc");
    const auto file = dao::core::gff3::File::Parse(bytes);
    REQUIRE(file);
    const auto root = file->Root();
    REQUIRE(root);
    CHECK(root->String("Tag") == "red_ogre");
    CHECK(root->Child("Appearance Data")->Get<std::uint32_t>("Appearance_Type") == 7701U);
    CHECK(root->ResRef("Conversation") == "ogre_talk"); // the rest kept

    const std::string to = ghilana::PathText(out);
    CHECK_THAT(
        Refusal(served, "template_edit", {{"name", "ogre.utc"}, {"set", {{"Nope", 1}}}, {"save_as", "x"}, {"to", to}}),
        ContainsSubstring("no field Nope"));
    CHECK_THAT(
        Refusal(served, "template_edit", {{"name", "ogre.utc"}, {"set", {{"Tag", 5}}}, {"save_as", "x"}, {"to", to}}),
        ContainsSubstring("another kind of value"));
}

TEST_CASE("model_copy: a model's files under a new name, each naming the new ones", "[ghilana][templates]")
{
    Served served;
    served.install.root.Write(L"documents/packages/core/override/slab.mmh", SlabModel());
    served.install.root.Write(L"documents/packages/core/override/slab.msh", SlabMesh());
    const auto out = served.install.root.path / L"out";
    const Json answer = served.Call(
        "model_copy", {{"name", "slab"}, {"as", "slab_red"}, {"material", "red"}, {"to", ghilana::PathText(out)}});
    CHECK(answer["written"] == Json::array({"slab_red.mmh", "slab_red.msh"}));
    const auto bytes = FileBytes(out / L"slab_red.mmh");
    const auto file = dao::core::gff4::File::Parse(bytes);
    REQUIRE(file);
    const auto tree = model::HierarchyOf(*file);
    REQUIRE(tree);
    CHECK(tree->name == L"slab_red.mmh");
    CHECK(tree->meshFile == L"slab_red.msh");
    for (const auto &node : tree->nodes)
    {
        if (node.mesh)
        {
            CHECK(node.mesh->material == L"red");
        }
    }
    CHECK_THAT(Refusal(served, "model_copy", {{"name", "slab"}, {"as", "bad name"}, {"to", ghilana::PathText(out)}}),
               ContainsSubstring("letters, digits and _"));
}

TEST_CASE("table_rows: a table of two words merges with its own name's files, not its first word's",
          "[ghilana][templates]")
{
    Served served;
    const std::vector<std::string> columns{"ID", "Label", "Cost"};
    served.install.root.Write(L"documents/packages/core/override/abi_cost.gda",
                              Gda(columns, {{1, std::string("One"), 1.0F}}, Hashes(columns)));
    served.install.root.Write(L"documents/packages/core/override/abi_cost_x.gda",
                              Gda(columns, {{5, std::string("Five"), 1.0F}}, Hashes(columns)));
    const std::string to = ghilana::PathText(served.install.root.path / L"out");
    const auto rows = [](int id) { return Json::array({{{"copy", 1}, {"set", {{"ID", id}}}}}); };
    const Json answer =
        served.Call("table_rows", {{"table", "abi_cost"}, {"rows", rows(9)}, {"name", "abi_cost_mine"}, {"to", to}});
    CHECK(answer["merged_into"] == "abi_cost");
    // 5 is a mod's abi_cost_x row; 3 is abi's (abi_mymod), another table.
    CHECK_THAT(Refusal(served, "table_rows",
                       {{"table", "abi_cost"}, {"rows", rows(5)}, {"name", "abi_cost_mine"}, {"to", to}}),
               ContainsSubstring("is taken"));
    CHECK(Refusal(served, "table_rows",
                  {{"table", "abi_cost"}, {"rows", rows(3)}, {"name", "abi_cost_mine2"}, {"to", to}})
              .empty());
    CHECK_THAT(
        Refusal(served, "table_rows", {{"table", "abi_cost"}, {"rows", rows(9)}, {"name", "abi_mine"}, {"to", to}}),
        ContainsSubstring("abi_cost_<yours>"));
}

TEST_CASE("table_rows: an item variation's ID is a byte", "[ghilana][templates]")
{
    Served served;
    const std::vector<std::string> columns{"ID", "Label", "Cost"};
    served.install.root.Write(L"documents/packages/core/override/sword_variation.gda",
                              Gda(columns, {{1, std::string("One"), 1.0F}}, Hashes(columns)));
    const std::string to = ghilana::PathText(served.install.root.path / L"out");
    const Json rows = Json::array({{{"copy", 1}, {"set", {{"ID", 256}}}}});
    CHECK_THAT(Refusal(served, "table_rows",
                       {{"table", "sword_variation"}, {"rows", rows}, {"name", "sword_variation_mine"}, {"to", to}}),
               ContainsSubstring("0 to 255"));
}
