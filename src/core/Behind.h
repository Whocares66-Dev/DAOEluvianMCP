// A launched game kept behind the window in front (#175): what becomes of
// the window calls by which the game brings itself forward, in a copy the
// bridge launched (ELUVIAN_BEHIND). The calls are user32's, detoured in
// src/game/Behind.cpp; what each becomes is decided here, over the numbers
// winuser.h gives them, so it is tested without the game.
//
// Two copies came to the front on 2026-10-24 and took the user's mouse, each
// right after a force quit of the copy that was there (elgara-agent): the
// game's own SetForegroundWindow, which Windows grants when no window is in
// front. The launch's SW_SHOWNOACTIVATE governs only its first ShowWindow.
#pragma once

#include <cstdint>

namespace eluvian
{

// ShowWindow's command in a copy kept behind: one that would activate the
// window shows it without, in the state it asked for; one that hides,
// minimises or already does not activate is left as it is.
int BehindShowCommand(int command);

// SetWindowPos's flags in a copy kept behind: never activating
// (SWP_NOACTIVATE) and never moved in the z-order (SWP_NOZORDER), so not
// raised above the window in front, whatever it asked.
std::uint32_t BehindPositionFlags(std::uint32_t flags);

} // namespace eluvian
