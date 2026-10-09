#include "core/Gift.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

// A follower the game gives a gift to some reads after it was signalled, as
// the gift event is handled a frame or more on.
struct Follower
{
    Bench bench{BenchOptions{std::chrono::milliseconds(2000)}};
    int approval = 7;
    int rise = 10;      // what the gift adds
    bool moves = true;  // whether the item leaves the pack
    int takenAfter = 3; // state reads before the game has taken it; -1 never
    std::string mode = "explore";
    bool refuse = false; // gift.give refuses (not a gift, not a follower)
    int reads = 0;

    Follower()
    {
        ToolSpec give{"gift.give", "", {}, Thread::Listener};
        give.listed = false;
        bench.Tools().Add(give, [this](const Json &args) -> ToolResult {
            if (refuse)
            {
                throw ToolError("gift: 0x500 is not a gift");
            }
            return Json{{"to", {{"id", args.value("to", "")}}},
                        {"item", {{"id", args.value("item", "")}}},
                        {"approvalBefore", approval},
                        {"cursor", 0}};
        });
        ToolSpec state{"gift.state", "", {}, Thread::Listener};
        state.listed = false;
        bench.Tools().Add(state, [this](const Json &) -> ToolResult {
            ++reads;
            const bool taken = takenAfter >= 0 && reads > takenAfter;
            return Json{{"approval", taken ? approval + rise : approval}, {"held", !(taken && moves)}};
        });
        bench.Tools().Add(ToolSpec{"health", "", {}, Thread::Listener},
                          [this](const Json &) -> ToolResult { return Json{{"mode", mode}}; });
        AddGiftTool(bench, std::chrono::milliseconds(1));
    }

    Json Gift(Json args = Json::object())
    {
        args.update(Json{{"to", "0x7d9"}, {"item", "0x500"}});
        return bench.Invoke("gift", args).data;
    }
};

} // namespace

TEST_CASE("a gift is answered once the game has taken it, not as it is signalled", "[gift]")
{
    Follower leliana;
    const auto answer = leliana.Gift();
    CHECK(answer["approvalBefore"] == 7);
    CHECK(answer["approvalAfter"] == 17);
    CHECK(answer["given"] == true);
    CHECK(leliana.reads == 4); // read until it was taken, and no longer
    CHECK_FALSE(answer.contains("note"));
}

TEST_CASE("an approval moved is a gift taken, the item still read in the pack", "[gift]")
{
    Follower wynne;
    wynne.moves = false;
    const auto answer = wynne.Gift();
    CHECK(answer["approvalAfter"] == 17);
    CHECK(answer["given"] == false);
    CHECK_FALSE(answer.contains("note"));
}

TEST_CASE("a gift the game never takes is said when the budget runs out", "[gift]")
{
    Follower alistair;
    alistair.takenAfter = -1;
    const auto answer = alistair.Gift(Json{{"budgetMs", 100}});
    CHECK(answer["given"] == false);
    CHECK(answer["approvalAfter"] == 7);
    CHECK_THAT(answer["note"].get<std::string>(), ContainsSubstring("had not taken the gift"));
    CHECK(answer["waitedMs"].get<int>() >= 100);
}

TEST_CASE("a gift that starts a conversation says so", "[gift]")
{
    Follower leliana; // Andraste's Grace
    leliana.mode = "dialog";
    const auto answer = leliana.Gift();
    CHECK(answer["mode"] == "dialog");
    CHECK_THAT(answer["note"].get<std::string>(), ContainsSubstring("conversation"));
}

TEST_CASE("a gift the game refuses is refused, nothing read after", "[gift]")
{
    Follower zevran;
    zevran.refuse = true;
    CHECK_THROWS_WITH(zevran.Gift(), ContainsSubstring("not a gift"));
    CHECK(zevran.reads == 0);
}

TEST_CASE("a gift that leaves the pack is taken though the approval stays, at its most", "[gift]")
{
    Follower alistair;
    alistair.approval = 100;
    alistair.rise = 0;
    const auto answer = alistair.Gift();
    CHECK(answer["given"] == true);
    CHECK(alistair.reads == 4); // seen as it left, not waited for to the budget's end
    CHECK_FALSE(answer.contains("note"));
}
