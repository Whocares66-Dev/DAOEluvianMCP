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

// Whether a copy kept behind, finding its own window in front, gives the
// front back: unless the user's own input brought it there, a mouse button
// or Alt or Win pressed in the last half second (a click on the window or
// the taskbar, Alt-Tab, Win and a number), and at most a few times a
// minute, so that a window that hands it straight back does not trade it
// for ever. A copy still reached the front by no call the detours see, once
// in sixteen launches after a force quit (elgara-agent, 2026-10-24, #179),
// and once at a menu a minute and a half after its load (2026-10-25), when
// the give-back was the first minute's only. The user, 2026-10-25, relayed:
// "If you can't fix the bring to front, just let it do so and close it
// afterwards."
// @param msSinceUserInput since a mouse button, Alt or Win was last seen down
// @param givenBackThisMinute times it gave the front back in the last minute
bool BehindGivesFrontBack(std::uint64_t msSinceUserInput, std::uint32_t givenBackThisMinute);

} // namespace eluvian
