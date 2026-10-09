// A list a Flash movie hands back: GFx gives an ActionScript array only as
// its text, its elements comma-joined (dao/Gui.h's Answer), as the engine's
// tactics FSCommands leave theirs in _root.fsc_retval (lib-agent, read in
// tactics.gfx's DAExternalCommands.as, 2026-10-08, #89).
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace eluvian
{

// The numbers of a comma-joined list, in order: "3,17,44" gives 3, 17 and
// 44. Blanks around an element are let be; an element that is no number
// ("", "undefined", "null") is left out.
std::vector<std::int64_t> ListNumbers(std::string_view joined);

} // namespace eluvian
