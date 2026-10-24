// level, atmosphere, stage and cutscene (#163) over a level, a stage and a
// cutscene laid out as DAOLib's tests of their readers lay them out
// (LevelFiles.h), put in the install's loose override.
#include "Fixture.h"
#include "LevelFiles.h"
#include "model_builder.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace eluvian;
using namespace ghtest;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace
{

constexpr const wchar_t *kOverride = L"documents/packages/core/override/";

struct Level
{
    Served served;

    Level()
    {
        const auto put = [&](const wchar_t *name, const Bytes &bytes) {
            served.install.root.Write(std::wstring(kOverride) + name, bytes);
        };
        put(L"lak303d.arl", levels::LevelLayoutFile());
        put(L"lak303d_3672.rml", levels::RoomFile());
        put(L"lak303d_3672_ll.mmh", levels::LayoutFile());
        put(L"slab.mmh", SlabModel());
        put(L"jowan_attack.stg", levels::StageFile());
        put(L"bhmcs_intro.cut", levels::CutsceneFile());
        // The campaign's own table, which the game searches; a table of another name it does not (#169).
        put(L"singleplayer_en-us.tlk", Tlk({{6081234, "Jowan, stop!"}}));
    }
};

void CheckPoint(const Json &point, float x, float y, float z)
{
    REQUIRE(point.size() == 3);
    CHECK(point[0].get<double>() == Approx(x).margin(1e-5));
    CHECK(point[1].get<double>() == Approx(y).margin(1e-5));
    CHECK(point[2].get<double>() == Approx(z).margin(1e-5));
}

} // namespace

TEST_CASE("level: the rooms, the models they place in the level's space, and the layouts' lights", "[ghilana][levels]")
{
    Level level;
    const Json out = level.served.Call("level", {{"name", "LAK303D"}});
    CHECK(out["id"] == 7);
    CHECK(out["name"] == "Circle Tower");
    CHECK(out["layout"]["name"] == "lak303d.arl");
    REQUIRE(out["rooms"].size() == 1);
    const Json &room = out["rooms"][0];
    CHECK(room["id"] == 3672);
    CHECK(room["file"] == "lak303d_3672");
    CheckPoint(room["lower"], -1, -2, -3);
    CheckPoint(room["at"], 100, 0, 0);
    CHECK(room["turn"][2].get<double>() == Approx(0.70710678));
    CHECK(room["sees"] == Json::array({7, 9}));
    CHECK(room["connects"] == Json::array({9, 12}));
    CHECK(room["models"] == 2); // the layout is no model of the room's

    // The layout is the room's lights, not a placed model.
    CHECK(out["count"] == 2);
    REQUIRE(out["placed"].size() == 2);
    const Json &slab = out["placed"][0];
    CHECK(slab["room"] == 3672);
    CHECK(slab["model"] == "slab");
    CheckPoint(slab["at"], 100, 0, 5);
    CHECK(slab["scale"] == 2.0);
    CHECK(slab["lightmaps"][0]["atlas"] == "room.dds");
    CHECK(slab["lightmaps"][0]["part"] == "_1");
    CHECK_FALSE(slab.contains("materials"));
    // (1, 1, 0) in the room, which is turned a quarter about z: (-1, 1, 0) from its place; and its own
    // quarter turn, then the room's, a half turn.
    const Json &torch = out["placed"][1];
    CHECK(torch["model"] == "fxe_torch");
    CheckPoint(torch["at"], 99, 1, 0);
    CHECK(torch["turn"][2].get<double>() == Approx(1.0));
    CHECK(torch["turn"][3].get<double>() == Approx(0.0).margin(1e-6));

    CHECK(out["lights"] == Json{{"lamps", 1}, {"ambients", 1}, {"probes", 1}});
    CHECK_FALSE(out.contains("lamps"));
}

TEST_CASE("level: `model` keeps the models whose name has a word, `materials` reads each one's parts",
          "[ghilana][levels]")
{
    Level level;
    const Json out = level.served.Call("level", {{"name", "lak303d"}, {"model", "SLA"}, {"materials", true}});
    CHECK(out["count"] == 1);
    REQUIRE(out["placed"].size() == 1);
    const Json &materials = out["placed"][0]["materials"];
    REQUIRE(materials.size() == 1);
    CHECK(materials[0] == Json{{"node", "Mesh1"}, {"part", "_1"}, {"chunk", "Body"}, {"material", "ogre"}});

    const Json torch = level.served.Call("level", {{"name", "lak303d"}, {"model", "torch"}, {"materials", true}});
    REQUIRE(torch["placed"].size() == 1);
    CHECK_THAT(torch["placed"][0]["materials"]["missing"].get<std::string>(), ContainsSubstring("fxe_torch.mmh"));

    const Json none = level.served.Call("level", {{"name", "lak303d"}, {"room", 7}});
    CHECK(none["count"] == 0);
    CHECK(none["lights"]["lamps"] == 0);
    CHECK(none["rooms"].size() == 1); // the rooms are all listed whatever is kept
}

TEST_CASE("level: `lights` lists the lamps, ambient lights and probes, each in the level's space", "[ghilana][levels]")
{
    Level level;
    const Json out = level.served.Call("level", {{"name", "lak303d"}, {"lights", true}});
    REQUIRE(out["lamps"].size() == 1);
    const Json &lamp = out["lamps"][0];
    CHECK(lamp["name"] == "Lamp_RT01");
    CHECK(lamp["room"] == 3672);
    CHECK(lamp["layout"] == "lak303d_3672_ll");
    // (1, 2, 0) in the layout (DAOLib's test of LayoutOf), turned with the room a quarter about z:
    // (-2, 1, 0) from the room's place.
    CheckPoint(lamp["at"], 98, 1, 0);
    CHECK(lamp["radius"] == 8.0);
    CHECK(lamp["affect_domain"] == 2);
    CHECK(lamp["static"] == true);
    CHECK(lamp["flicker"]["variation"].get<double>() == Approx(0.25));
    CHECK(lamp["flicker"]["period"].get<double>() == Approx(0.075));
    REQUIRE(out["ambients"].size() == 1);
    CHECK(out["ambients"][0]["name"] == "Ambient0");
    CHECK(out["ambients"][0]["colour"][0].get<double>() == Approx(0.104));
    REQUIRE(out["probes"].size() == 1);
    const Json &probe = out["probes"][0];
    CheckPoint(probe["at"], 100, 1, 3);
    CHECK(probe["cube"] == "lak303d_probe_2zz.dds");
    CHECK(probe["irradiance_file"] == "lak303d_probe_2zz.mtx");
    CHECK(probe["baked"] == true);
}

TEST_CASE("level: a name the game has no layout of is refused, saying where levels are listed", "[ghilana][levels]")
{
    Level level;
    CHECK_THROWS_WITH(level.served.Call("level", {{"name", "nowhere"}}),
                      ContainsSubstring("`resources` with type arl"));
    CHECK_THROWS_WITH(level.served.Call("atmosphere", {{"level", "lak303d_3672.rml"}}),
                      ContainsSubstring("no level layout"));
}

TEST_CASE("atmosphere: the sun, the atmosphere's values by name, the frame effects and the sky dome",
          "[ghilana][levels]")
{
    Level level;
    const Json out = level.served.Call("atmosphere", {{"level", "lak303d"}});
    CHECK(out["name"] == "Circle Tower");
    CheckPoint(out["sun"]["direction"], 0, 0, -1);
    CHECK(out["sun"]["colour"] == Json::array({0.5, 0.25, 0.125, 1.0}));
    CHECK_FALSE(out["sun"].contains("colour_on_characters"));
    CHECK(out["atmosphere"]["AtmoSunIntensity"].get<double>() == Approx(19.95));
    CHECK(out["atmosphere"]["22999"] == 3.0); // a field DAOLib has no name for keeps its number
    CHECK(out["clouds"] == Json::object());
    REQUIRE(out["frame_effects"].size() == 1);
    CHECK(out["frame_effects"][0]["name"] == "da_hsvmatrix");
    CHECK(out["frame_effects"][0]["parameters"] == Json{{"Brightness", 0.75}});
    CHECK(out["sky_dome"].is_null());
}

TEST_CASE("stage: its places and cameras, each field by its own", "[ghilana][levels]")
{
    Level level;
    const Json out = level.served.Call("stage", {{"name", "jowan_attack"}});
    REQUIRE(out["places"].size() == 2);
    CHECK(out["places"][0]["tag"] == "duncan");
    CHECK(out["places"][0]["position"] == Json::array({-3.0, 0.25, 0.5}));
    CHECK(out["places"][0]["default_camera"] == "group");
    CHECK_FALSE(out["places"][1].contains("default_camera"));
    REQUIRE(out["cameras"].size() == 1);
    const Json &camera = out["cameras"][0];
    CHECK(camera["tag"] == "group");
    CHECK(camera["fov"] == 45.0);
    CHECK(camera["places_in_shot"] == Json::array({"duncan", "irving"}));
    CHECK(camera["looking_from"] == "duncan");
    CHECK(camera["looking_at_primary"] == "irving");
    CHECK(camera["looking_at_secondary"] == "greagoir");
    CHECK(camera["looking_at_type"] == 3);
}

TEST_CASE("cutscene: its header, its actors, and each action by kind with its own fields and its line's text",
          "[ghilana][levels]")
{
    Level level;
    const Json out = level.served.Call("cutscene", {{"name", "bhmcs_intro"}});
    CHECK(out["area"] == "bhm500ar_tower_harrowing");
    CHECK(out["run_time"] == 61.0);
    CHECK(out["fps"] == 30);
    CHECK(out["end_script"] == "gen00cs_cutscene_end");
    CHECK(out["area_required"] == true);
    CHECK(out["henchman_tag"] == "gen00fl_alistair");
    CHECK_FALSE(out.contains("stage"));
    REQUIRE(out["actors"].size() == 1);
    const Json &jowan = out["actors"][0];
    CHECK(jowan["id"] == 7);
    CHECK(jowan["name"] == "Jowan");
    CHECK(jowan["mapping_tag"] == "bhm_jowan");
    CHECK(jowan["origin"]["position"] == Json::array({1.0, 2.0, 3.0}));
    CHECK_FALSE(jowan.contains("placement_at"));
    const Json &actions = jowan["actions"];
    REQUIRE(actions.size() == 3);
    CHECK(actions[0]["kind"] == "AANI");
    CHECK(actions[0]["what"] == "animation");
    CHECK(actions[0]["animation"] == "mag_cast");
    CHECK(actions[0]["start"] == 2.0);
    CHECK(actions[0]["stop"] == "end");
    CHECK(actions[0]["speed"] == 0.5);
    CHECK(actions[0]["gad"]["play"] == true);
    CHECK(actions[0]["gad"]["position_keys"] == 2);
    CHECK(actions[0]["curves"] == 1);
    CHECK_FALSE(actions[0].contains("curve_keys"));
    CHECK(actions[1]["what"] == "spoken line");
    CHECK(actions[1]["stop"] == 5.5);
    CHECK(actions[1]["vo_bank"] == "vo_bhm_jowan");
    CHECK(actions[1]["line"] == Json{{"strref", 6081234}, {"text", "Jowan, stop!"}});
    CHECK(actions[2]["what"] == "stage camera");
    CHECK(actions[2]["tag"] == "gen00fl_alistair");
    CHECK(actions[2]["stage_camera_henchman"] == true);
    CHECK_FALSE(actions[2].contains("animation"));
}

TEST_CASE("cutscene: `curves` gives the keys, `at` every curve's value and every actor's place then",
          "[ghilana][levels]")
{
    Level level;
    const Json out =
        level.served.Call("cutscene", {{"name", "bhmcs_intro.cut"}, {"kind", "aani"}, {"curves", true}, {"at", 10.01}});
    CHECK(out["clock_at"].get<double>() == Approx(10.0)); // the nearest of its 30 frames a second
    const Json &actions = out["actors"][0]["actions"];
    REQUIRE(actions.size() == 1);
    const Json &curve = actions[0]["curve_keys"][0];
    CHECK(curve["base"] == -6.5);
    CHECK(curve["vertices"] == Json::array({Json::array({41.5, -20.25})}));
    // One vertex: its value at every time.
    CHECK(actions[0]["values_at"] == Json::array({-20.25}));
    REQUIRE(out["actors"][0].contains("placement_at"));
    CHECK(out["actors"][0]["placement_at"]["position"].size() == 3);

    CHECK(level.served.Call("cutscene", {{"name", "bhmcs_intro"}, {"actor", "nobody"}})["actors"].empty());
    CHECK(level.served.Call("cutscene", {{"name", "bhmcs_intro"}, {"actor", 7}})["actors"].size() == 1);
    CHECK(level.served.Call("cutscene", {{"name", "bhmcs_intro"}, {"actor", "JOWAN"}})["actors"].size() == 1);
    CHECK_THROWS_WITH(level.served.Call("cutscene", Json::object()), ContainsSubstring("`conversation`"));
}
