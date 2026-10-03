#include "core/Args.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("ids are written as the logs write them", "[args]")
{
    CHECK(FormatId(0x7d9) == "0x7d9");
    CHECK(FormatId(0) == "0x0");
    CHECK(FormatId(0xFFFFFFFF) == "0xffffffff");
}

TEST_CASE("ids are read from hex, decimal and numbers", "[args]")
{
    CHECK(ParseId(Json("0x7d9")) == 0x7d9u);
    CHECK(ParseId(Json("0X7D9")) == 0x7d9u);
    CHECK(ParseId(Json("2009")) == 2009u);
    CHECK(ParseId(Json(2009)) == 2009u);
    CHECK(ParseId(Json("0xffffffff")) == 0xFFFFFFFFu);
    CHECK_FALSE(ParseId(Json("7d9")));
    CHECK_FALSE(ParseId(Json("0x")));
    CHECK_FALSE(ParseId(Json("0x1ffffffff")));
    CHECK_FALSE(ParseId(Json(-1)));
    CHECK_FALSE(ParseId(Json(4294967296ULL)));
    CHECK_FALSE(ParseId(Json(1.5)));
    CHECK_FALSE(ParseId(Json(nullptr)));
}

TEST_CASE("arguments must be an object, or nothing", "[args]")
{
    CHECK_NOTHROW(Args(Json(nullptr), "t"));
    CHECK_THROWS_WITH(Args(Json::array(), "inspect"), ContainsSubstring("inspect: the arguments must be an object"));
}

TEST_CASE("each reader has a fallback and a legible failure", "[args]")
{
    const Args args(Json{{"name", "Aedan"}, {"radius", 12.5}, {"limit", 3}, {"all", true}, {"none", nullptr}},
                    "inspect");
    CHECK(args.String("name") == "Aedan");
    CHECK(args.String("missing", "x") == "x");
    CHECK(args.String("none", "null reads as absent") == "null reads as absent");
    CHECK_THROWS_WITH(args.String("radius"), ContainsSubstring("inspect: `radius` must be given, as a string"));
    CHECK(args.Number("radius") == 12.5);
    CHECK(args.Number("missing", 2.0) == 2.0);
    CHECK_THROWS_AS(args.Number("name"), ToolError);
    CHECK(args.Integer("limit", 10, 1, 100) == 3);
    CHECK(args.Integer("missing", 10, 1, 100) == 10);
    CHECK_THROWS_WITH(args.Integer("limit", 10, 5, 100), ContainsSubstring("a whole number from 5 to 100"));
    CHECK_THROWS_WITH(args.Integer("limit", 10, 1, 2), ContainsSubstring("a whole number from 1 to 2"));
    CHECK_THROWS_AS(args.Integer("radius", 10, 1, 100), ToolError);
    CHECK_THROWS_AS(args.Integer("name", 10, 1, 100), ToolError);
    CHECK(args.Bool("all", false));
    CHECK_FALSE(args.Bool("missing", false));
    CHECK_THROWS_AS(args.Bool("name", false), ToolError);
}

TEST_CASE("an id argument says what it should name", "[args]")
{
    const Args args(Json{{"id", "0x7d9"}, {"bad", "Howe Archer"}}, "order");
    CHECK(args.Id("id", "a creature") == 0x7d9u);
    CHECK_FALSE(args.OptionalId("missing", "a creature"));
    CHECK_THROWS_WITH(args.Id("bad", "a creature, as inspect kind=creatures lists them"),
                      ContainsSubstring("order: `bad` must be an object id like \"0x7d9\": a creature, as inspect "
                                        "kind=creatures lists them"));
}

TEST_CASE("a choice lists the choices", "[args]")
{
    const Args args(Json{{"action", "attack"}, {"bad", "smite"}}, "order");
    CHECK(args.Choice("action", {"attack", "move"}) == "attack");
    CHECK(args.Choice("missing", {"attack", "move"}, "move") == "move");
    CHECK_THROWS_WITH(args.Choice("bad", {"attack", "move"}), ContainsSubstring("one of: attack, move"));
}
