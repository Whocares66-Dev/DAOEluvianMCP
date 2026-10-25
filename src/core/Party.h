// `party`: the party picker, the screen the game shows after a recruit and
// at camp, read and driven as a player does: who it offers and who is
// chosen, a follower chosen or taken out as a click on them does, Remove
// All, and Accept. Before it a playthrough driven by MCP alone could only
// accept the party as it stood (turn-based-agent, 2026-10-03, #25).
//
// Since #182 each step is DAOLib's dao::partypicker (DAOLib #259), through
// the unlisted party.entries, party.choose, party.release,
// party.release_all and party.accept, refused as the picker refuses the
// click. Until then a follower was chosen by holding the cursor at pixel
// after pixel across the picker's scene, which has its own camera, until
// PartyPicker.MouseOverName was theirs, then sending the click's binding
// there (as turn-based-agent found Sten, 2026-10-03), and the rest by
// GameModeController's texts. It decides over other tools' answers alone
// (those, `health`, `script` and `state`), so it lives here and is tested
// with stand-ins.
#pragma once

#include "core/Bench.h"

namespace eluvian
{

void AddPartyTool(Bench &bench);

} // namespace eluvian
