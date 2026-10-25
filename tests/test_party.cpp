#include "core/Party.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// The party picker as DAOLib's dao::partypicker answers it (#182): its
// entries, a choice and a release refused as the picker refuses the click,
// Remove All and Accept; the party the scripts make after.
struct Camp
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    struct Member
    {
        std::string id;
        std::string name;
        std::string tag;
        std::string state; // chosen, available, unavailable, suspended, lockedActive
        int slot;
        bool hero;
    };
    std::vector<Member> members = {{"0x82", "Lyna", "player", "chosen", 0, true},
                                   {"0x10d", "Alistair", "gen00fl_alistair", "chosen", 1, false},
                                   {"0x110", "Morrigan", "gen00fl_morrigan", "available", -1, false},
                                   {"0x111", "Dog", "gen00fl_dog", "available", -1, false},
                                   {"0x112", "Sten", "gen00fl_sten", "unavailable", -1, false}};
    std::string mode = "partypicker";
    int status = 2;     // GetPartyPickerGUIStatus: USE
    int emptyReads = 0; // entries read empty before the picker is done showing
    bool closes = true; // whether Accept closes the picker
    std::vector<std::string> party = {"Lyna", "Alistair"};
    std::vector<std::string> accepted; // the party the scripts make after Accept
    int lag = 0;                       // state's reads after Accept before it has it
    std::vector<std::string> calls;    // the picker's steps, in order

    Camp()
    {
        bench.Tools().Add(ToolSpec{"health", "", {}, Thread::Listener},
                          [this](const Json &) -> ToolResult { return Json{{"mode", mode}}; });
        bench.Tools().Add(ToolSpec{"script", "", {}, Thread::Listener},
                          [this](const Json &) -> ToolResult { return Json{{"value", status}}; });
        bench.Tools().Add(ToolSpec{"state", "", {}, Thread::Listener}, [this](const Json &) -> ToolResult {
            if (!accepted.empty() && lag-- <= 0)
            {
                party = accepted;
                accepted.clear();
            }
            Json listed = Json::array();
            for (const auto &name : party)
            {
                listed.push_back(Json{{"name", name}});
            }
            return Json{{"party", listed}};
        });
        bench.Tools().Add(ToolSpec{"party.entries", "", {}, Thread::Listener},
                          [this](const Json &) -> ToolResult { return Entries(); });
        for (const char *step : {"party.choose", "party.release", "party.release_all", "party.accept"})
        {
            bench.Tools().Add(ToolSpec{step, "", {}, Thread::Listener},
                              [this, step = std::string(step)](const Json &args) -> ToolResult {
                                  calls.push_back(step + (args.contains("id") ? " " + args.at("id").get<std::string>()
                                                                              : std::string()));
                                  return Step(step, args.value("id", ""));
                              });
        }
        AddPartyTool(bench);
    }

    Json Entries()
    {
        Json entries = Json::array();
        if (emptyReads > 0)
        {
            --emptyReads;
            return Json{{"entries", entries}};
        }
        for (const auto &member : members)
        {
            entries.push_back(Json{{"id", member.id},
                                   {"name", member.name},
                                   {"tag", member.tag},
                                   {"state", member.state},
                                   {"slot", member.slot},
                                   {"hero", member.hero}});
        }
        return Json{{"entries", entries}};
    }

    static Json Refused(const char *refusal)
    {
        return Json{{"taken", false}, {"refusal", refusal}};
    }

    int Chosen() const
    {
        int count = 0;
        for (const auto &member : members)
        {
            count += member.slot >= 0 ? 1 : 0;
        }
        return count;
    }

    Json Step(const std::string &step, const std::string &id)
    {
        if (step == "party.accept")
        {
            for (const auto &member : members)
            {
                if (member.slot >= 0)
                {
                    accepted.push_back(member.name);
                }
            }
            mode = closes ? "explore" : mode;
            return Json{{"taken", true}};
        }
        if (step == "party.release_all")
        {
            for (auto &member : members)
            {
                if (member.slot >= 0 && !member.hero && member.state != "lockedActive")
                {
                    member.slot = -1;
                    member.state = "available";
                }
            }
            return Json{{"taken", true}};
        }
        for (auto &member : members)
        {
            if (member.id == id)
            {
                return Click(step == "party.choose", member);
            }
        }
        return Refused("not offered");
    }

    // A click on a follower: chosen, or let go.
    Json Click(bool choose, Member &member) const
    {
        if (member.hero || member.state == "lockedActive" || member.state == "unavailable")
        {
            return Refused("locked");
        }
        if (choose == (member.slot >= 0))
        {
            return Refused("already so");
        }
        if (choose && Chosen() >= 4)
        {
            return Refused("party full");
        }
        member.slot = choose ? Chosen() : -1;
        member.state = choose ? "chosen" : "available";
        return Json{{"taken", true}};
    }

    Json Party(const Json &args)
    {
        return bench.Invoke("party", args).data;
    }
};

} // namespace

TEST_CASE("look says who is chosen, slot by slot, and everyone the picker offers with their state", "[party]")
{
    Camp camp;
    const auto answer = camp.Party(Json{{"action", "look"}});
    CHECK(answer["status"] == "use");
    REQUIRE(answer["chosen"].size() == 2);
    CHECK(answer["chosen"][1] == Json{{"slot", 1}, {"id", "0x10d"}, {"name", "Alistair"}, {"tag", "gen00fl_alistair"}});
    REQUIRE(answer["offered"].size() == 5);
    CHECK(answer["offered"][4]["state"] == "unavailable");
    CHECK_FALSE(answer.contains("before"));
    CHECK(camp.calls.empty());
}

TEST_CASE("look waits for the picker to be done showing", "[party]")
{
    Camp camp;
    camp.emptyReads = 3;
    const auto answer = camp.Party(Json{{"action", "look"}});
    CHECK(answer["chosen"].size() == 2);
}

TEST_CASE("a picker that shows no one is said", "[party]")
{
    Camp camp;
    camp.emptyReads = 1000;
    CHECK_THROWS_WITH(camp.Party(Json{{"action", "look"}}), ContainsSubstring("showed no one after 5 s"));
}

TEST_CASE("choose picks a follower by name in any case, as a click on them does", "[party]")
{
    Camp camp;
    const auto answer = camp.Party(Json{{"action", "choose"}, {"name", "dog"}});
    CHECK(camp.calls == std::vector<std::string>{"party.choose 0x111"});
    REQUIRE(answer["chosen"].size() == 3);
    CHECK(answer["chosen"][2]["name"] == "Dog");
    CHECK(answer["before"].size() == 2);
}

TEST_CASE("choose and unchoose take a tag or an id as well as a name", "[party]")
{
    Camp camp;
    camp.Party(Json{{"action", "choose"}, {"name", "gen00fl_morrigan"}});
    camp.Party(Json{{"action", "unchoose"}, {"name", "0x10d"}});
    CHECK(camp.calls == std::vector<std::string>{"party.choose 0x110", "party.release 0x10d"});
}

TEST_CASE("a follower the picker does not show is refused, naming those it does", "[party]")
{
    Camp camp;
    CHECK_THROWS_WITH(camp.Party(Json{{"action", "choose"}, {"name", "Zevran"}}),
                      ContainsSubstring("shows no one named Zevran; it shows Lyna (chosen), Alistair (chosen)"));
    CHECK(camp.calls.empty());
}

TEST_CASE("each refusal of the picker's is said in words", "[party]")
{
    Camp camp;
    CHECK_THROWS_WITH(camp.Party(Json{{"action", "choose"}, {"name", "Alistair"}}),
                      ContainsSubstring("Alistair was not chosen: chosen already"));
    CHECK_THROWS_WITH(camp.Party(Json{{"action", "choose"}, {"name", "Sten"}}),
                      ContainsSubstring("Sten was not chosen: the story keeps them where they are"));
    CHECK_THROWS_WITH(camp.Party(Json{{"action", "unchoose"}, {"name", "Morrigan"}}),
                      ContainsSubstring("Morrigan was not taken out: not chosen"));
    CHECK_THROWS_WITH(camp.Party(Json{{"action", "unchoose"}, {"name", "Lyna"}}),
                      ContainsSubstring("Lyna was not taken out: the story keeps them"));
    camp.Party(Json{{"action", "choose"}, {"name", "Morrigan"}});
    camp.Party(Json{{"action", "choose"}, {"name", "Dog"}});
    camp.members[4].state = "available";
    CHECK_THROWS_WITH(camp.Party(Json{{"action", "choose"}, {"name", "Sten"}}),
                      ContainsSubstring("Sten was not chosen: four are chosen: unchoose one first"));
}

TEST_CASE("clear lets go all but the hero and those the story keeps, and says when it took no one", "[party]")
{
    Camp camp;
    camp.members[2].slot = 2;
    camp.members[2].state = "lockedActive";
    auto answer = camp.Party(Json{{"action", "clear"}});
    REQUIRE(answer["chosen"].size() == 2);
    CHECK(answer["chosen"][1]["name"] == "Morrigan");
    CHECK_FALSE(answer.contains("note"));
    answer = camp.Party(Json{{"action", "clear"}});
    CHECK_THAT(answer["note"].get<std::string>(), ContainsSubstring("took no one out"));
}

TEST_CASE("accept waits for the party to hold everyone chosen", "[party]")
{
    Camp camp;
    camp.lag = 2; // the scripts change it a frame or more after Accept
    camp.Party(Json{{"action", "choose"}, {"name", "Dog"}});
    const auto answer = camp.Party(Json{{"action", "accept"}});
    CHECK(answer["accepted"] == true);
    CHECK(answer["mode"] == "explore");
    CHECK(answer["party"].size() == 3);
}

TEST_CASE("a picker still up after Accept is said", "[party]")
{
    Camp camp;
    camp.closes = false;
    CHECK_THROWS_WITH(camp.Party(Json{{"action", "accept"}}), ContainsSubstring("still up 5 s after Accept"));
}

TEST_CASE("a picker only to look at refuses choices, saying how to open it to change the party", "[party]")
{
    Camp camp;
    camp.status = 1;
    CHECK_THROWS_WITH(camp.Party(Json{{"action", "choose"}, {"name", "Dog"}}),
                      ContainsSubstring("SetPartyPickerGUIStatus [2]"));
    CHECK(camp.Party(Json{{"action", "look"}})["status"] == "readOnly");
    CHECK(camp.calls.empty());
}

TEST_CASE("party refuses with the picker not up, or a choice with no name", "[party]")
{
    Camp camp;
    CHECK_THROWS_WITH(camp.Party(Json{{"action", "choose"}}), ContainsSubstring("needs `name`"));
    camp.mode = "explore";
    CHECK_THROWS_WITH(camp.Party(Json{{"action", "look"}}), ContainsSubstring("not up (the game is in mode explore)"));
}
