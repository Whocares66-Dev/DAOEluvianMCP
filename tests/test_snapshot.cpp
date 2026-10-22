#include "core/Snapshot.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A snapshot of two creatures, a plot and a variable, at 10 s.
Json Base()
{
    return Json::parse(R"({
        "schema": "eluvian/snapshot 1", "area": "bed100ar_forest_clearing", "mode": "explore", "paused": true,
        "gameTime": 10.0, "controlled": "player|default_player#0",
        "party": ["player|default_player#0", "tamlen|bed100cr_tamlen#0"],
        "creatures": {
            "player|default_player#0": {
                "key": "player|default_player#0", "name": "Lyna",
                "position": {"x": 1, "y": 2, "z": 3}, "facingDegrees": 179,
                "health": {"current": 100, "max": 100}, "stamina": {"current": 50, "max": 120},
                "dead": false, "group": 0, "combatState": 1, "attackTarget": null,
                "effects": [{"type": "EFFECT_TYPE_MODIFY_PROPERTY", "ability": null, "remaining": null},
                            {"type": "EFFECT_TYPE_DAMAGE", "ability": 3000, "remaining": 4.0}]
            },
            "tamlen|bed100cr_tamlen#0": {
                "key": "tamlen|bed100cr_tamlen#0", "position": {"x": 5, "y": 5, "z": 0},
                "health": {"current": 115, "max": 115}, "dead": false
            }
        },
        "plots": {"bed000pt_main": {"MET_TAMLEN": true, "FOUND_MIRROR": false}},
        "vars": [{"object": "module", "name": "MODULE_COUNTER_3", "type": "int", "value": 1}]
    })");
}

const Json &Find(const Json &answer, const std::string &path)
{
    for (const auto &difference : answer.at("differences"))
    {
        if (difference.at("path") == path)
        {
            return difference;
        }
    }
    FAIL_CHECK("no difference at " << path << " in " << answer.dump());
    static const Json none = Json::object();
    return none;
}

} // namespace

TEST_CASE("snapshot: creatures of one tag and template are counted in the order they were made")
{
    const auto keys = CreatureKeys(
        {{"genlock", "genlock_dagger"}, {"player", "default_player"}, {"genlock", "genlock_dagger"}, {"", "x"}});
    CHECK(keys == std::vector<std::string>{"genlock|genlock_dagger#0", "player|default_player#0",
                                           "genlock|genlock_dagger#1", "|x#0"});
}

TEST_CASE("snapshot_diff: a snapshot is the same as itself")
{
    const auto answer = SnapshotDiff(Base(), Base(), {});
    CHECK(answer.at("same") == true);
    CHECK(answer.at("count") == 0);
    CHECK(answer.at("differences").empty());
    CHECK(answer.at("compared").at("creatures") == 2);
    CHECK(answer.at("compared").at("flags") == 2);
    CHECK(answer.at("compared").at("vars") == 1);
}

TEST_CASE("snapshot_diff: a position is a distance, within the tolerance the same")
{
    Json b = Base();
    b["creatures"]["player|default_player#0"]["position"] = {{"x", 1.005}, {"y", 2}, {"z", 3}};
    CHECK(SnapshotDiff(Base(), b, {}).at("same") == true);
    b["creatures"]["player|default_player#0"]["position"] = {{"x", 4}, {"y", 6}, {"z", 3}};
    const auto answer = SnapshotDiff(Base(), b, {});
    CHECK(answer.at("count") == 1);
    const auto &found = Find(answer, "/creatures/player|default_player#0/position");
    CHECK(found.at("by").get<double>() == 5.0);
    CHECK(found.at("a").at("x") == 1);
    CHECK(found.at("b").at("x") == 4);
}

TEST_CASE("snapshot_diff: facing is measured the short way round")
{
    Json b = Base();
    b["creatures"]["player|default_player#0"]["facingDegrees"] = -180.5;
    CHECK(SnapshotDiff(Base(), b, {}).at("same") == true);
    b["creatures"]["player|default_player#0"]["facingDegrees"] = 170;
    const auto answer = SnapshotDiff(Base(), b, {});
    const auto &found = Find(answer, "/creatures/player|default_player#0/facingDegrees");
    CHECK(found.at("by").get<double>() == 9.0);
}

TEST_CASE("snapshot_diff: health and stamina, current and max, by their tolerances")
{
    Json b = Base();
    b["creatures"]["player|default_player#0"]["health"]["current"] = 99;
    b["creatures"]["player|default_player#0"]["stamina"]["max"] = 121;
    auto answer = SnapshotDiff(Base(), b, {});
    CHECK(answer.at("count") == 2);
    CHECK(Find(answer, "/creatures/player|default_player#0/health/current").at("by") == 1.0);
    Find(answer, "/creatures/player|default_player#0/stamina/max");

    SnapshotTolerance loose;
    loose.health = 1;
    loose.stamina = 1;
    CHECK(SnapshotDiff(Base(), b, loose).at("same") == true);
}

TEST_CASE("snapshot_diff: any other field both give is compared as it is")
{
    Json b = Base();
    b["creatures"]["player|default_player#0"]["dead"] = true;
    b["creatures"]["player|default_player#0"]["attackTarget"] = "genlock|genlock_dagger#0";
    b["creatures"]["player|default_player#0"]["name"] = "Lyna in another language";
    b["creatures"]["player|default_player#0"]["id"] = "0x82"; // the engine's own, not compared
    const auto answer = SnapshotDiff(Base(), b, {});
    CHECK(answer.at("count") == 2);
    CHECK_FALSE(answer.contains("notCompared"));
    Find(answer, "/creatures/player|default_player#0/dead");
    Find(answer, "/creatures/player|default_player#0/attackTarget");
}

TEST_CASE("snapshot_diff: a field only one side gives is not compared, and says so")
{
    Json b = Base();
    b["creatures"]["player|default_player#0"].erase("stamina");
    b["creatures"]["tamlen|bed100cr_tamlen#0"]["command"] = {{"type", 1}};
    const auto answer = SnapshotDiff(Base(), b, {});
    CHECK(answer.at("same") == true);
    const auto &notCompared = answer.at("notCompared");
    CHECK(std::ranges::find(notCompared, Json("/creatures/*/stamina")) != notCompared.end());
    CHECK(std::ranges::find(notCompared, Json("/creatures/*/command")) != notCompared.end());
}

TEST_CASE("snapshot_diff: a creature only one side has")
{
    Json b = Base();
    b["creatures"].erase("tamlen|bed100cr_tamlen#0");
    b["creatures"]["genlock|genlock_dagger#0"] = {{"position", {{"x", 0}, {"y", 0}, {"z", 0}}}};
    const auto answer = SnapshotDiff(Base(), b, {});
    CHECK(answer.at("count") == 2);
    CHECK(Find(answer, "/creatures/tamlen|bed100cr_tamlen#0").at("b").is_null());
    CHECK(Find(answer, "/creatures/genlock|genlock_dagger#0").at("a").is_null());
    CHECK(answer.at("compared").at("creatures") == 1);
}

TEST_CASE("snapshot_diff: effects are matched by type and ability, their time left by the time's tolerance")
{
    Json b = Base();
    auto &effects = b["creatures"]["player|default_player#0"]["effects"];
    effects[1]["remaining"] = 4.04;
    CHECK(SnapshotDiff(Base(), b, {}).at("same") == true);
    effects[1]["remaining"] = 3.0;
    effects.push_back({{"type", "EFFECT_TYPE_STUN"}, {"ability", 10}, {"remaining", 2}});
    // The same effects in another order are the same.
    std::swap(effects[0], effects[1]);
    const auto answer = SnapshotDiff(Base(), b, {});
    CHECK(answer.at("count") == 2);
    for (const auto &difference : answer.at("differences"))
    {
        const auto path = difference.at("path").get<std::string>();
        CHECK_THAT(path, ContainsSubstring("/effects/"));
    }
    const auto &added = answer.at("differences")[1];
    CHECK(added.at("a").is_null());
    CHECK(added.at("b").at("type") == "EFFECT_TYPE_STUN");
}

TEST_CASE("snapshot_diff: a game time apart is one skew, said first")
{
    Json b = Base();
    b["gameTime"] = 10.5;
    const auto answer = SnapshotDiff(Base(), b, {});
    CHECK(answer.at("skew").get<double>() == 0.5);
    CHECK(answer.at("differences")[0].at("path") == "/gameTime");
    CHECK_THAT(answer.at("note").get<std::string>(), ContainsSubstring("skew"));
    b["gameTime"] = 10.04;
    CHECK(SnapshotDiff(Base(), b, {}).contains("skew") == false);
}

TEST_CASE("snapshot_diff: the area, mode, pause and the one controlled, as they are; the party as a set")
{
    Json b = Base();
    b["party"] = {"tamlen|bed100cr_tamlen#0", "player|default_player#0"};
    CHECK(SnapshotDiff(Base(), b, {}).at("same") == true);
    b["party"].push_back("merrill|x#0");
    b["mode"] = "combat";
    b["controlled"] = "tamlen|bed100cr_tamlen#0";
    const auto answer = SnapshotDiff(Base(), b, {});
    CHECK(answer.at("count") == 3);
    Find(answer, "/party");
    Find(answer, "/mode");
    Find(answer, "/controlled");
}

TEST_CASE("snapshot_diff: plot flags and variables; one only one side gives is a difference")
{
    Json b = Base();
    b["plots"]["bed000pt_main"]["FOUND_MIRROR"] = true;
    b["plots"]["bed000pt_main"]["NEW_FLAG"] = false;
    b["vars"][0]["value"] = 221;
    const auto answer = SnapshotDiff(Base(), b, {});
    CHECK(answer.at("count") == 3);
    CHECK(Find(answer, "/plots/bed000pt_main/FOUND_MIRROR").at("b") == true);
    CHECK(Find(answer, "/plots/bed000pt_main/NEW_FLAG").at("a").is_null());
    CHECK(Find(answer, "/vars/module~1MODULE_COUNTER_3").at("b") == 221);
}

TEST_CASE("snapshot_diff: at most `most` listed, `count` has them all")
{
    Json b = Base();
    b["mode"] = "combat";
    b["paused"] = false;
    b["area"] = "elsewhere";
    const auto answer = SnapshotDiff(Base(), b, {}, 2);
    CHECK(answer.at("count") == 3);
    CHECK(answer.at("differences").size() == 2);
    CHECK(answer.at("truncated") == true);
}

TEST_CASE("snapshot_diff: the tolerance as given, over the defaults")
{
    const auto tolerance = ToleranceFrom(Json{{"position", 0.5}, {"time", 0}});
    CHECK(tolerance.position == 0.5);
    CHECK(tolerance.time == 0.0);
    CHECK(tolerance.facing == 1.0);
    CHECK_THROWS_WITH(ToleranceFrom(Json{{"speed", 1}}), ContainsSubstring("no `speed`"));
    CHECK_THROWS_WITH(ToleranceFrom(Json{{"health", -1}}), ContainsSubstring("0 or more"));
    CHECK_THROWS_AS(ToleranceFrom(Json::array()), ToolError);
}

TEST_CASE("snapshot_diff: a snapshot given as itself or as a file")
{
    CHECK(SnapshotFrom(Base(), "a") == Base());
    const auto path = std::filesystem::temp_directory_path() / "eluvian_test_snapshot_from.json";
    {
        std::ofstream file(path, std::ios::binary);
        file << Base().dump();
    }
    CHECK(SnapshotFrom(Json(path.string()), "a") == Base());
    {
        std::ofstream file(path, std::ios::binary);
        file << "[1, 2]";
    }
    CHECK_THROWS_WITH(SnapshotFrom(Json(path.string()), "b"), ContainsSubstring("`b`"));
    std::filesystem::remove(path);
    CHECK_THROWS_WITH(SnapshotFrom(Json(path.string()), "a"), ContainsSubstring("no file"));
    CHECK_THROWS_AS(SnapshotFrom(Json(3), "a"), ToolError);
}

TEST_CASE("snapshot_diff: the tool compares two snapshots it is given, and wants both")
{
    Bench bench;
    AddSnapshotDiffTool(bench);
    Json b = Base();
    b["mode"] = "combat";
    const auto answer = bench.Invoke("snapshot_diff", Json{{"a", Base()}, {"b", b}}).data;
    CHECK(answer.at("count") == 1);
    CHECK_THROWS(bench.Invoke("snapshot_diff", Json{{"a", Base()}}));
}
