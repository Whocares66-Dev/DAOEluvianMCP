// Ghilana's tools over an install made in the test (Fixture.h), called as an
// agent calls them, through the bench.
#include "Fixture.h"

#include "ghilana/Text.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace eluvian;
using namespace ghtest;
using Catch::Matchers::ContainsSubstring;

namespace
{

const Json *SourceIn(const Json &sources, std::string_view id)
{
    for (const auto &s : sources)
    {
        if (s["id"].get<std::string>() == id)
        {
            return &s;
        }
    }
    return nullptr;
}

std::string Message(Served &served, std::string_view tool, const Json &args)
{
    try
    {
        served.Call(tool, args);
    }
    catch (const ToolError &e)
    {
        return e.what();
    }
    return {};
}

} // namespace

TEST_CASE("environment: the sources found, their archives and files, the add-ins' settings", "[ghilana][tools]")
{
    Served served;
    const Json env = served.Call("environment");
    CHECK(env["index"]["built"] == true);
    CHECK(env["language"] == "en-us");
    const Json &sources = env["sources"];
    const Json *core = SourceIn(sources, "core");
    REQUIRE(core != nullptr);
    CHECK((*core)["archives"] == 5); // 2da, creatures, two texture packs, a patch
    CHECK((*core)["loose"] == 2);    // the two talk tables; the .fsb is not a resource
    const Json *addin = SourceIn(sources, "addin:dao_prc_cp_1");
    REQUIRE(addin != nullptr);
    CHECK((*addin)["unreadable"] == 1);
    CHECK((*addin)["files"] == 1); // its manifest.xml is not a resource
    CHECK((*addin)["settings"]["title"] == "Return to Ostagar");
    CHECK((*addin)["settings"]["enabled"] == true);
    CHECK(SourceIn(sources, "campaign") != nullptr);
    CHECK(SourceIn(sources, "plugin:mymod") != nullptr);
    const Json *docs = SourceIn(sources, "override:documents");
    REQUIRE(docs != nullptr);
    CHECK((*docs)["in"] == "documents");
    CHECK(env["unreadable"]["enciphered_archives"] == 1);
    CHECK(env["game_state"]["campaign"] == "Single Player");
    CHECK(env["game_state"]["textures"] == "high");
    CHECK_THAT(env["precedence"].get<std::string>(), ContainsSubstring("lowest priority"));
    REQUIRE(env["listed_without_folder"].size() == 1);
    CHECK(env["listed_without_folder"][0]["uid"] == "DAO_PRC_GONE");
    // The index is built once: a second call does not say it built it.
    CHECK_FALSE(served.Call("environment").contains("index"));
}

TEST_CASE("a package a campaign loads is a source of that campaign, ahead of the core", "[ghilana][tools]")
{
    // packages\core_ep1 was no source at all, so Awakening's copies were
    // missing from every answer and the core's taken (lib-agent, 2026-10-13,
    // #129). Its campaign's .cif names it; the main campaign's does not.
    Served origins;
    Json env = origins.Call("environment");
    CHECK(env["game_state"]["packages"].empty());
    REQUIRE(SourceIn(env["sources"], "package:core_ep1") != nullptr);
    const Json golem = origins.Call("conflicts", {{"name", "golem.utc"}});
    CHECK(golem["taken"].is_null());
    CHECK_THAT(golem["why"].get<std::string>(), ContainsSubstring("another campaign"));
    Json read = origins.Call("resource", {{"name", "hurlock.utc"}});
    CHECK(read["copy"]["source"] == "override:documents");

    Served awakening("DAO_PRC_EP_1");
    env = awakening.Call("environment");
    CHECK(env["game_state"]["packages"] == Json::array({"core_ep1"}));
    read = awakening.Call("resource", {{"name", "golem.utc"}});
    CHECK(read["copy"]["source"] == "package:core_ep1");
    CHECK(read["taken"] == true);
    // Ahead of the core's copy, behind Documents' loose override.
    const Json c = awakening.Call("conflicts", {{"name", "hurlock.utc"}});
    CHECK(c["taken"]["source"] == "override:documents");
    std::vector<std::string> order;
    for (const auto &copy : c["copies"])
    {
        order.push_back(copy["source"].get<std::string>());
    }
    const auto ep1 = std::ranges::find(order, "package:core_ep1");
    const auto core = std::ranges::find(order, "core");
    REQUIRE(ep1 != order.end());
    REQUIRE(core != order.end());
    CHECK(ep1 < core);
    CHECK(order.front() == "override:documents");
}

TEST_CASE("resources: by words or a pattern, with every copy and whether it reads", "[ghilana][tools]")
{
    Served served;
    Json found = served.Call("resources", {{"match", "ogre"}});
    REQUIRE(found["count"] == 3); // ogre.utc, ogre_d.dds, ogre_talk.dlg
    CHECK(found["resources"][0]["name"] == "ogre.utc");
    found = served.Call("resources", {{"match", "ogre"}, {"type", "dds"}});
    REQUIRE(found["count"] == 1);
    // In the game's order: the plugin's, the core's high quality, the medium
    // one, which the game does not have while it plays at high.
    const Json &copies = found["resources"][0]["copies"];
    REQUIRE(copies.size() == 3);
    CHECK(copies[0]["source"] == "plugin:mymod");
    CHECK(copies[0]["priority"] == 2);
    CHECK(copies[1]["archive"] == "packages/core/textures/high/texturepack.erf");
    CHECK_FALSE(copies[1].contains("unused"));
    CHECK_THAT(copies[2]["unused"].get<std::string>(), ContainsSubstring("texture quality"));
    found = served.Call("resources", {{"match", "abi_*.gda"}});
    CHECK(found["count"] == 5);
    found = served.Call("resources", {{"match", "abi_cp1"}});
    REQUIRE(found["count"] == 1);
    CHECK(found["resources"][0]["copies"][0]["readable"] == false);
    found = served.Call("resources", {{"source", "plugin:mymod"}});
    CHECK(found["count"] == 2);
    found = served.Call("resources", {{"limit", 2}, {"offset", 1}});
    CHECK(found["resources"].size() == 2);
    CHECK(found.contains("more"));
    CHECK_THAT(Message(served, "resources", {{"source", "nowhere"}}), ContainsSubstring("environment"));
}

TEST_CASE("resource: a template's fields, a table's rows, a talk table, a texture's size", "[ghilana][tools]")
{
    Served served;
    Json read = served.Call("resource", {{"name", "ogre.utc"}});
    CHECK(read["format"] == "gff3:utc");
    CHECK(read["content"]["Appearance Data"]["Appearance_Type"] == 80);
    CHECK(read["content"]["Conversation"] == "ogre_talk");
    CHECK_FALSE(read.contains("chosen"));

    read = served.Call("resource", {{"name", "abi_base.gda"}});
    CHECK(read["format"] == "gda");
    CHECK(read["content"]["row_count"] == 2);
    CHECK(read["content"]["rows"][1]["Label"] == "Dual");

    read = served.Call("resource", {{"name", "core_en-us.tlk"}});
    CHECK(read["format"] == "tlk");
    CHECK(read["content"]["count"] == 3);

    read = served.Call("resource", {{"name", "ogre_talk.dlg"}});
    CHECK(read["format"] == "text");
    CHECK(read["content"]["text"] == "not read");
}

TEST_CASE("resource: of several copies the one the game takes; another named, with the game's said", "[ghilana][tools]")
{
    Served served;
    Json read = served.Call("resource", {{"name", "ogre_d.dds"}});
    CHECK(read["copy"]["source"] == "plugin:mymod");
    CHECK(read["taken"] == true);
    CHECK(read["content"]["width"] == 4);
    CHECK(read["content"]["format"] == "B8G8R8A8");
    CHECK(read["content"]["mips"] == 2);
    REQUIRE(read["other_copies"].size() == 2);
    read = served.Call("resource", {{"name", "ogre_d.dds"}, {"source", "core"}});
    CHECK(read["copy"]["source"] == "core");
    CHECK(read["taken"] == false);
    CHECK(read["the_game_takes"]["source"] == "plugin:mymod");
    CHECK(read["content"]["width"] == 8);
    // The core's patch over its data.
    read = served.Call("resource", {{"name", "abi_base.gda"}});
    CHECK(read["copy"]["archive"] == "packages/core/patch/patch001.erf");
    CHECK(read["content"]["rows"][0]["Label"] == "Flurry patched");
    read = served.Call("resource",
                       {{"name", "ogre_d.dds"}, {"archive", R"(packages\core\textures\high\texturepack.erf)"}});
    CHECK(read["copy"]["source"] == "core");
    // A loose copy in Documents' override over the core's.
    read = served.Call("resource", {{"name", "hurlock.utc"}});
    CHECK(read["copy"]["source"] == "override:documents");
    CHECK(read["content"]["Appearance Data"]["Appearance_Type"] == 82);
}

TEST_CASE("resource: what is not there or cannot be read is said, with what to do", "[ghilana][tools]")
{
    Served served;
    CHECK_THAT(Message(served, "resource", {{"name", "abi_base"}}), ContainsSubstring("abi_base.gda"));
    CHECK_THAT(Message(served, "resource", {{"name", "nothing.utc"}}), ContainsSubstring("resources"));
    CHECK_THAT(Message(served, "resource", {{"name", "abi_cp1.gda"}}), ContainsSubstring("enciphered"));
    CHECK_THAT(Message(served, "resource", {{"name", "ogre.utc"}, {"source", "campaign"}}),
               ContainsSubstring("its copies are"));
}

TEST_CASE("conflicts: the copy the game takes and every copy in its order", "[ghilana][tools]")
{
    Served served;
    Json c = served.Call("conflicts", {{"name", "ogre_d.dds"}});
    CHECK(c["taken"]["source"] == "plugin:mymod");
    CHECK(c["copies"].size() == 3); // one resource: every copy, the medium texture's too
    c = served.Call("conflicts");
    CHECK(c["count"] == 2); // ogre_d.dds and hurlock.utc: copies the game has in two sources each
    c = served.Call("conflicts", {{"name", "hurlock.utc"}});
    CHECK(c["taken"]["source"] == "override:documents");
    c = served.Call("conflicts", {{"source", "plugin:mymod"}});
    REQUIRE(c["count"] == 1);
    CHECK(c["conflicts"][0]["name"] == "ogre_d.dds");
    CHECK(c["lost"] == 0);
    c = served.Call("conflicts", {{"source", "core"}});
    CHECK(c["count"] == 2);
    CHECK(c["lost"] == 2); // the plugin's texture and Documents' creature win over the core's
    // The ones the game does not have: the medium texture, the disabled add-in's creature.
    c = served.Call("conflicts", {{"unused", true}});
    CHECK(c["count"] == 3);
    c = served.Call("conflicts", {{"name", "ogre.utc"}});
    CHECK(c["taken"]["archive"] == "packages/core/data/designercreatures.erf");
    REQUIRE(c["copies"].size() == 2);
    CHECK_THAT(c["copies"][1]["unused"].get<std::string>(), ContainsSubstring("not enabled"));
    // A resource the game has no copy of says why.
    c = served.Call("conflicts", {{"name", "off_only.uti"}});
    CHECK(c["taken"].is_null());
    CHECK_THAT(c["why"].get<std::string>(), ContainsSubstring("not enabled"));
}

TEST_CASE("table: an M2DA as the game merges it, an ID's first file's row kept", "[ghilana][tools]")
{
    Served served;
    // The game's order of the abilities' files: the install's override (1), the plugin's (2),
    // the add-in's (enciphered), the core's patch (100000), whose abi_base.gda shadows the core's
    // data's.
    Json t = served.Call("table", {{"table", 1}});
    CHECK(t["table"]["label"] == "Abilities");
    REQUIRE(t["files"].size() == 4);
    CHECK(t["files"][2]["file"] == "abi_mymod.gda");
    CHECK(t["files"][3]["file"] == "abi_base.gda");
    REQUIRE(t["unreadable"]["files"].size() == 1);
    CHECK(t["unreadable"]["files"][0]["file"] == "abi_cp1.gda");
    REQUIRE(t["shadowed"].size() == 1);
    CHECK(t["shadowed"][0]["copy"]["archive"] == "packages/core/data/2da.erf");
    CHECK(t["count"] == 4); // 7 from the override, 2 and 3 from the mod, 1 from the patch
    CHECK(t["overrides"]["ids"] == 2);
    // ID 7: two files of one folder, so the first by name wins.
    t = served.Call("table", {{"table", "abilities"}, {"id", 7}});
    REQUIRE(t["rows"].size() == 1);
    CHECK(t["rows"][0]["Label"] == "Over A");
    CHECK(t["rows"][0]["_over"] == Json::array({"abi_ovr_b.gda"}));

    // ID 2: the mod's row, over the patch's.
    t = served.Call("table", {{"table", "abilities"}, {"id", 2}});
    REQUIRE(t["rows"].size() == 1);
    CHECK(t["rows"][0]["Label"] == "Dual Mod");
    CHECK(t["rows"][0]["_source"] == "plugin:mymod");
    CHECK(t["rows"][0]["_in"] == "bin_ship/daolib/plugins/mymod/override/abi_mymod.gda");
    CHECK(t["rows"][0]["_over"] == Json::array({"abi_base.gda"}));
    CHECK_FALSE(t["rows"][0].contains("_unsure"));
    // With `dropped`, the patch's row the game drops too.
    t = served.Call("table", {{"table", "abilities"}, {"id", 2}, {"dropped", true}});
    REQUIRE(t["rows"].size() == 2);
    CHECK(t["rows"][1]["_dropped"] == true);
    CHECK(t["rows"][1]["_in"] == "packages/core/patch/patch001.erf");
    // The patch's own row comes after the enciphered add-in's file in the game's order.
    t = served.Call("table", {{"table", "abilities"}, {"id", 1}});
    REQUIRE(t["rows"].size() == 1);
    CHECK(t["rows"][0]["Label"] == "Flurry patched");
    CHECK(t["rows"][0]["_unsure"] == true);

    t = served.Call("table", {{"table", "abi"}, {"where", {{"label", "new"}}}, {"columns", {"ID"}}});
    REQUIRE(t["rows"].size() == 1);
    CHECK(t["rows"][0]["ID"] == 3);
    CHECK_FALSE(t["rows"][0].contains("Label"));
    t = served.Call("table", {{"table", "abi_base"}, {"overrides_only", true}});
    CHECK(t["count"] == 2);
    CHECK_THAT(Message(served, "table", {{"table", "nope"}}), ContainsSubstring("worksheet"));
    CHECK_THAT(Message(served, "table", {{"table", 1}, {"where", {{"Nope", 1}}}}), ContainsSubstring("Label"));
}

TEST_CASE("strings: ids to text across the language's talk tables, and text to ids", "[ghilana][tools]")
{
    Served served;
    Json s = served.Call("strings", {{"id", Json::array({100, 5000, 42})}});
    REQUIRE(s["strings"].size() == 3);
    CHECK(s["strings"][0]["text"] == "Ogre");
    CHECK(s["strings"][1]["text"] == "Hello");
    CHECK(s["strings"][1]["table"] == "singleplayer_en-us.tlk");
    CHECK(s["strings"][2]["text"].is_null());
    s = served.Call("strings", {{"text", "ogre"}});
    REQUIRE(s["count"] == 1);
    CHECK(s["strings"][0]["id"] == 100);
    s = served.Call("strings", {{"id", 100}, {"language", "fr-fr"}});
    CHECK(s["strings"][0]["text"] == "Ogre FR");
    CHECK_THAT(Message(served, "strings", {{"id", 1}, {"language", "xx-xx"}}), ContainsSubstring("tlk"));
}

TEST_CASE("query: templates by a field's value, the fields asked, counts by a field", "[ghilana][tools]")
{
    Served served;
    Json q = served.Call("query", {{"type", "utc"}, {"where", {{"Appearance_Type", 80}}}, {"fields", {"Tag"}}});
    REQUIRE(q["count"] == 1);
    CHECK(q["matches"][0]["name"] == "ogre.utc");
    CHECK(q["matches"][0]["fields"]["Tag"] == "ogre");
    // The copy the game takes of each: Documents' hurlock, not the core's it shadows.
    q = served.Call("query", {{"type", "utc"}, {"where", {{"LocName", 101}}}});
    CHECK(q["count"] == 1);
    CHECK(q["matches"][0]["copy"]["source"] == "override:documents");
    q = served.Call("query", {{"type", "utc"}, {"where", {{"LocName", 101}}}, {"copies", "all"}});
    CHECK(q["count"] == 4); // and core_ep1's hurlock and golem, another campaign's
    q = served.Call("query", {{"type", "utc"}, {"group_by", "Appearance_Type"}});
    CHECK(q["files_read"] == 2);
    CHECK(q["groups"].size() == 2);
    q = served.Call("query", {{"type", "utc"}, {"copies", "all"}});
    CHECK(q["files_read"] == 6); // and the disabled add-in's ogre, and core_ep1's two
    q = served.Call("query", {{"type", "utc"}, {"where", {{"appearance_type", Json::array({80, 82})}}}});
    CHECK(q["count"] == 2);
    q = served.Call("query", {{"type", "utc"}, {"where", {{"Tag", "OGRE"}}}, {"source", "core"}});
    CHECK(q["count"] == 1);
    // A dotted path names the nested field; a wrong one finds nothing.
    q = served.Call("query",
                    {{"type", "utc"}, {"where", {{"appearance data.appearance_type", 81}}}, {"copies", "all"}});
    CHECK(q["count"] == 1);
    q = served.Call("query", {{"type", "utc"}, {"where", {{"Tag.Appearance_Type", 81}}}});
    CHECK(q["count"] == 0);
}

TEST_CASE("references: what a template names, and what names a conversation", "[ghilana][tools]")
{
    Served served;
    Json r = served.Call("references", {{"name", "ogre.utc"}, {"source", "core"}});
    REQUIRE(r["uses"].size() == 1);
    CHECK(r["uses"][0]["field"] == "Conversation");
    CHECK(r["uses"][0]["resources"][0] == "ogre_talk.dlg");
    REQUIRE(r["strrefs"].size() == 1);
    CHECK(r["strrefs"][0]["id"] == 100);
    CHECK(r["used_by"].is_string());
    // ogre_talk.dlg is no GFF, so what names it is asked of a template.
    r = served.Call("references", {{"name", "hurlock.utc"}, {"source", "core"}, {"used_by_types", {"utc"}}});
    CHECK(r["used_by"].empty());
}

TEST_CASE("extract: files written into a folder of the caller's, never the game's", "[ghilana][tools]")
{
    Served served;
    const auto to = served.install.root.path / L"out";
    Json e = served.Call("extract", {{"pattern", "abi_*.gda"}, {"to", ghilana::PathText(to)}});
    CHECK(e["written"].size() == 4);
    REQUIRE(e["skipped"].size() == 1);
    CHECK_THAT(e["skipped"][0]["why"].get<std::string>(), ContainsSubstring("enciphered"));
    CHECK(std::filesystem::file_size(to / L"abi_base.gda") > 0);
    // Every copy, each under its source and its archive: the core has two.
    e = served.Call("extract", {{"names", {"ogre_d.dds"}}, {"to", ghilana::PathText(to)}, {"all_copies", true}});
    CHECK(e["written"].size() == 3);
    CHECK(std::filesystem::exists(to / L"plugin_mymod/bin_ship/daolib/plugins/mymod/override/ogre_d.dds"));
    CHECK(std::filesystem::exists(to / L"core/packages/core/textures/high/texturepack.erf/ogre_d.dds"));
    CHECK(std::filesystem::exists(to / L"core/packages/core/textures/medium/texturepack.erf/ogre_d.dds"));
    CHECK_THAT(Message(served, "extract",
                       {{"names", {"ogre.utc"}}, {"to", ghilana::PathText(served.install.game / L"packages")}}),
               ContainsSubstring("never writes"));
    CHECK_THAT(Message(served, "extract", {{"names", {"ogre.utc"}}, {"to", "relative"}}),
               ContainsSubstring("full path"));
}

TEST_CASE("texture: a DDS as a PNG image, its largest picture no larger than asked", "[ghilana][tools]")
{
    Served served;
    auto result = served.bench.Invoke("texture", {{"name", "ogre_d.dds"}, {"source", "core"}});
    CHECK(result.data["width"] == 8);
    // What the file holds, the shown picture apart from it (#130).
    CHECK(result.data["file"]["format"] == "B8G8R8A8");
    CHECK(result.data["file"]["width"] == 8);
    CHECK(result.data["file"]["mips"] == 2);
    CHECK(result.data["file"]["shape"] == "flat");
    CHECK_FALSE(result.data["file"].contains("header_mips")); // the test file's header says 2 too
    REQUIRE(result.content.size() == 1);
    CHECK(result.content[0]["type"] == "image");
    CHECK(result.content[0]["mimeType"] == "image/png");
    CHECK(result.content[0]["data"].get<std::string>().starts_with("iVBORw0KGgo")); // "\x89PNG" in base64
    result =
        served.bench.Invoke("texture", {{"name", "ogre_d.dds"}, {"source", "core"}, {"size", 4}, {"channel", "alpha"}});
    CHECK(result.data["width"] == 4);
    CHECK(result.data["file"]["width"] == 8);
    CHECK(result.data["channel"] == "alpha");
    CHECK_THAT(Message(served, "texture", {{"name", "ogre.utc"}}), ContainsSubstring("DDS"));
}

TEST_CASE("every tool's name is a model's and its schema is an object", "[ghilana][tools]")
{
    Served served;
    const Json tools = served.bench.Tools().Describe();
    CHECK(tools.size() == 13);
    for (const auto &tool : tools)
    {
        const auto name = tool["name"].get<std::string>();
        CHECK(name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") == std::string::npos);
        CHECK(tool["inputSchema"]["type"] == "object");
        CHECK(tool["description"].get<std::string>().size() > 80);
    }
}
