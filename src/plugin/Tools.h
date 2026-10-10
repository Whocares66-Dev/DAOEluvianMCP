// Every tool the bench serves, registered as the plugin registers them at
// load. The plugin calls it, and so does eluvian_describe, which prints the
// tools and topics for the docs site without the game (#109): one list, so
// the docs say what the game serves.
#pragma once

#include "core/Bench.h"
#include "game/Health.h"

#include <filesystem>
#include <functional>

namespace eluvian::plugin
{

struct ToolSetup
{
    // What a wait that times out says of why nothing came (a pause).
    std::function<Json()> whyNothing;
    // What health adds of the plugin's own: port, version, owner, plugins.
    std::function<Json()> health;
    // DAOLib's log folder, which `log` reads.
    std::filesystem::path logDirectory;
};

void RegisterTools(Bench &bench, const game::Watch &watch, const ToolSetup &setup);

} // namespace eluvian::plugin
