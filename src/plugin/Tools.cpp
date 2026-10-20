#include "plugin/Tools.h"

#include "core/CharGen.h"
#include "core/Conversation.h"
#include "core/Craft.h"
#include "core/Gift.h"
#include "core/LevelUp.h"
#include "core/Loot.h"
#include "core/Party.h"
#include "core/Ready.h"
#include "core/Scenario.h"
#include "core/Travel.h"
#include "core/Until.h"
#include "core/Warp.h"
#include "game/GameTools.h"

#include <chrono>

namespace eluvian::plugin
{

void RegisterTools(Bench &bench, const game::Watch &watch, const ToolSetup &setup)
{
    AddEventTools(bench, setup.whyNothing);
    AddLogTool(bench, setup.logDirectory);
    AddScenarioTool(bench, setup.whyNothing);
    AddWaitUntilTool(bench);
    AddWaitReadyTool(bench);
    AddGiftTool(bench);
    AddTravelTool(bench);
    AddWarpTool(bench);
    AddDialogueTool(bench);
    // A second on each screen a tool opens and closes in one call, so that
    // whoever watches sees it (the user, 2026-10-03, #14).
    AddLootTool(bench, std::chrono::seconds(1));
    AddLevelUpTool(bench, std::chrono::seconds(1));
    AddCraftTool(bench, std::chrono::seconds(1));
    AddCharGenTool(bench);
    AddPartyTool(bench);
    game::AddHealthTool(bench, watch, setup.health);
    game::AddGameTools(bench, watch);
}

} // namespace eluvian::plugin
