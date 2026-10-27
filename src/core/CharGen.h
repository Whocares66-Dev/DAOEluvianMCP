// `chargen`: build the hero on the game's own character creation screen, as
// a player's clicks do, so the engine and the scripts do what they do for a
// player: sys_chargen.nss rebuilds the hero from each choice, and the end
// sends the party to the origin's start (lib-agent's reading, DAOLib's
// dev/CHARGEN.md; asked for at the user's request, 2026-10-18, #153).
//
// Since #189 each step is DAOLib's dao::chargen (DAOLib #259): what the
// screen offers and each choice made as its buttons make it, refused where it
// greys it out, through the unlisted chargen.offers, chargen.select,
// chargen.name and chargen.finish; the points by levelup.raise and
// levelup.take in the creation screen's mode. Until then it sent the screen's
// GUICharGen messages itself (read from chargen.gfx, patch003's, and seen in
// play 2026-10-18), read the game's tables by script, named the hero through
// the movie's SetValueUnicode and ended the movie by setting its exit state,
// with two workarounds DAOLib's run found unneeded: the movie's first
// CharGenStart sent again with the hero, and the background box closed by
// press. It decides over other tools' answers alone (those, `character` and
// `gui`), so it lives here and is tested with stand-ins, as levelup is.
#pragma once

#include "core/Bench.h"

namespace eluvian
{

void AddCharGenTool(Bench &bench);

} // namespace eluvian
