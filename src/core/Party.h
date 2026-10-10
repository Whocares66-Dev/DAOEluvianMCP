// `party`: the party picker, the screen the game shows after a recruit and
// at camp, read and driven as a player does: who is chosen, a follower
// chosen in by a click on them in the picker's scene, one taken out by a
// click on their portrait, and Accept. Before it a playthrough driven by
// MCP alone could only accept the party as it stood (turn-based-agent,
// 2026-10-03, #25).
//
// The picker's own messages (DAOLib's message_handlers.csv) and movie
// (partypicker.gfx): the chosen are the data tree's
// PartyPicker.SelectedPartyMember.<0..3>, each with a Name and a Tag;
// "GameModeController.UnselectPartyMember <slot>" is a chosen portrait's
// click, "GameModeController.SubmitPartyChanges" Accept,
// "GameModeController.ClearPartyPicker" Clear; a click in the scene is the
// binding SelectPartyMemberUnderCursorMessage, for the follower under the
// cursor, whose name PartyPicker.MouseOverName holds. The scene has its own
// camera, so a follower's pixel is not the world's: it is found as
// turn-based-agent found Sten's, by holding the cursor at pixels across
// the view and reading the name under it (seen in play that day: the five
// at y 450 of a 1000-pixel view, Sten at 600). It decides over other tools'
// answers alone (`pointer`, `input`, `data`, `screen`,
// `health`), so it lives here and is tested with stand-ins.
#pragma once

#include "core/Bench.h"

#include <chrono>

namespace eluvian
{

// `settle`: how long the cursor is held at a pixel before the name under it
// is read; the game picks again about every fifteenth of a second, and
// turn-based-agent's sweep waited 0.3 s.
void AddPartyTool(Bench &bench, std::chrono::milliseconds settle = std::chrono::milliseconds(300));

} // namespace eluvian
