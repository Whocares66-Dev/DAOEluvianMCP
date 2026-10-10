// eluvian_describe: what the game serves, without the game. Registers every
// tool as the plugin does at load (plugin/Tools.h) and every topic Eluvian
// emits (game::DescribeTopics), touching no game memory, and prints them as
// JSON: `tools` as MCP's tools/list gives them, `topics` as `events
// describe=true` does. The docs site's reference is built from it (#109),
// so what it says is what tools/list says. A plugin's own tools and topics
// are not here: they are the plugin's to document.
#include "core/Bench.h"
#include "game/GameTools.h"
#include "game/Health.h"
#include "plugin/Tools.h"

#include <cstdio>
#include <exception>

int main()
{
    try
    {
        eluvian::Bench bench;
        const eluvian::game::Watch watch(bench);
        eluvian::plugin::RegisterTools(bench, watch, eluvian::plugin::ToolSetup{{}, {}, {}});
        eluvian::game::DescribeTopics(bench.Topics());
        const eluvian::Json out = {{"version", EL_VERSION_STRING},
                                   {"tools", bench.Tools().Describe()},
                                   {"topics", bench.Topics().List(eluvian::EventFilter{})}};
        std::fputs(out.dump(1).c_str(), stdout);
        std::fputs("\n", stdout);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "eluvian_describe: %s\n", error.what());
        return 1;
    }
}
