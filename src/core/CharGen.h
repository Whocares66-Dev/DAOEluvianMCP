// `chargen`: build the hero on the game's own character creation screen, as
// a player's clicks do, so the engine and the scripts do what they do for a
// player: sys_chargen.nss rebuilds the hero from each choice, and the end
// sends the party to the origin's start (lib-agent's reading, DAOLib's
// dev/CHARGEN.md; asked for at the user's request, 2026-10-18, #153).
//
// The screen's messages, read from chargen.gfx (patch003's) by a subagent
// and seen in play 2026-10-18 in instance eluvian-test: GUICharGen's
// CharGenStart, CharGenSelectGender, Race, Class and Background ("<creature>
// <old> <new> 1"), CharGenSelectAppearanceAndSoundSet, CharGenAssignAttribute
// and CharGenAssignAbility, the name in GUICharGen.TempCharName with
// CharGenSelectName, and the end as the movie ends it. It decides over other
// tools' answers alone (`input` sends, `movie` drives the screen's script,
// `data` and `character` read the hero, `script` the game's tables), so it
// lives here and is tested with stand-ins, as levelup is.
#pragma once

#include "core/Bench.h"

namespace eluvian
{

void AddCharGenTool(Bench &bench);

} // namespace eluvian
