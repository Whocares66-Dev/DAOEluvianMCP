// The game's reads for a stand-in: a fake written for inspect's kinds,
// registered under each read's own tool (#110), called with `kind` set as
// inspect's was. The stand-ins keep their one fake; the tools they stand in
// for are the ones the core now asks.
#pragma once

#include "core/Bench.h"

#include <functional>
#include <string>
#include <utility>

namespace eluvian
{

inline void AddReads(Bench &bench, const std::function<ToolResult(const Json &)> &fake)
{
    // Each read's tool and the kind it was of inspect.
    static const std::pair<const char *, const char *> kReads[] = {
        {"state", "game"},
        {"creatures", "creatures"},
        {"creature", "creature"},
        {"placeables", "placeables"},
        {"exits", "exits"},
        {"triggers", "triggers"},
        {"surroundings", "surroundings"},
        {"geometry", "geometry"},
        {"character", "character"},
        {"abilities", "abilities"},
        {"equipment", "equipment"},
        {"inventory", "inventory"},
        {"item", "item"},
        {"data", "data"},
        {"plot", "plot"},
        {"journal", "journal"},
        {"resources", "resources"},
        {"gui", "gui"},
        {"cursor", "cursor"},
        {"window", "window"},
        {"controls", "controls"},
        {"worldmap", "worldmap"},
        {"dialogue.read", "dialogue"},
        {"tactics.read", "tactics"},
    };
    for (const auto &[tool, kind] : kReads)
    {
        bench.Tools().Add(ToolSpec{tool, "", {}, Thread::Listener},
                          [fake, kind = std::string(kind)](const Json &args) -> ToolResult {
                              Json asked = args.is_object() ? args : Json::object();
                              // worldmap's `all` was inspect's maps.
                              asked["kind"] = kind == "worldmap" && asked.value("all", false) ? "maps" : kind;
                              return fake(asked);
                          });
    }
}

} // namespace eluvian
