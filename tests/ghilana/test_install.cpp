// Ghilana over a real install, where this machine has one: answers known
// from the game's files (DAOLib's tools/gda.py and tools/tlk.py read the
// same, 2026-09-30). Skipped where no install is found.
#include "ghilana/Folders.h"
#include "ghilana/Ghilana.h"

#include <catch2/catch_test_macros.hpp>

using namespace eluvian;
using namespace eluvian::ghilana;

namespace
{

struct Real
{
    std::optional<Ghilana> ghilana;
    Bench bench;

    Real()
    {
        const auto installs = FindInstalls();
        if (installs.empty())
        {
            return;
        }
        Paths paths;
        paths.game = installs.front().folder;
        paths.documents = GameDocuments().value_or(fs::path());
        ghilana.emplace(paths, installs, fs::temp_directory_path() / L"ghilana_test_cache", "test");
        RegisterTools(bench, *ghilana);
    }
};

Real &Shared()
{
    static Real real; // the index read once for every test here
    return real;
}

} // namespace

TEST_CASE("install: the core's archives are found and indexed", "[ghilana][install]")
{
    auto &real = Shared();
    if (!real.ghilana)
    {
        SKIP("no game install on this machine");
    }
    const Json env = real.bench.Invoke("environment", Json::object()).data;
    bool core = false;
    for (const auto &source : env["sources"])
    {
        if (source["id"] == "core")
        {
            core = true;
            CHECK(source["archives"].get<int>() > 100);
        }
    }
    CHECK(core);
    CHECK(env["unreadable"]["enciphered_archives"].get<int>() >= 0);
}

TEST_CASE("install: table 1 is the abilities, abi_base among its files", "[ghilana][install]")
{
    auto &real = Shared();
    if (!real.ghilana)
    {
        SKIP("no game install on this machine");
    }
    const Json t = real.bench.Invoke("table", Json{{"table", 1}, {"limit", 1}}).data;
    CHECK(t["table"]["label"] == "Abilities");
    bool base = false;
    for (const auto &file : t["files"])
    {
        base = base || file["file"] == "abi_base.gda";
    }
    CHECK(base);
    CHECK(t["count"].get<int>() > 500);
}

TEST_CASE("install: a creature template is read, and the core's talk table names things", "[ghilana][install]")
{
    auto &real = Shared();
    if (!real.ghilana)
    {
        SKIP("no game install on this machine");
    }
    const Json r = real.bench.Invoke("resources", Json{{"type", "utc"}, {"source", "core"}, {"limit", 1}}).data;
    REQUIRE(r["count"].get<int>() > 100);
    const std::string name = r["resources"][0]["name"];
    const Json read = real.bench.Invoke("resource", Json{{"name", name}, {"source", "core"}}).data;
    CHECK(read["format"] == "gff3:utc");
    const Json s = real.bench.Invoke("strings", Json{{"text", "Flurry"}, {"exact", true}}).data;
    CHECK(s["count"].get<int>() >= 1);
}

TEST_CASE("install: merged rows as the game had them in play", "[ghilana][install]")
{
    // Seen in GOG's game by lib-agent, through Eluvian's script tool (DAOLib's dev/FILES.md, Merged
    // tables): GetM2DAInt(268, "TitleStrRef", 25) is Awakening's 445854, over the core patch's -1;
    // GetM2DARows(10076), aip_archer, a single table, is 12.
    auto &real = Shared();
    if (!real.ghilana)
    {
        SKIP("no game install on this machine");
    }
    const Json row = real.bench.Invoke("table", Json{{"table", 268}, {"id", 25}}).data;
    REQUIRE(row["rows"].size() == 1);
    CHECK(row["rows"][0]["TitleStrRef"] == 445854);
    CHECK(row["rows"][0]["_source"] == "addin:dao_prc_ep_1");
    const Json archer = real.bench.Invoke("table", Json{{"table", 10076}, {"limit", 1}}).data;
    CHECK(archer["table"]["single"] == true);
    CHECK(archer["count"] == 12);
}
