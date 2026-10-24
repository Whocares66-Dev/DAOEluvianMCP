// A copy of the game the bridge launched kept behind the window in front
// (#175): user32's SetForegroundWindow, ShowWindow and SetWindowPos
// detoured in this process, so the game cannot bring itself forward and
// take the mouse and keys of whoever works in front. What each call becomes
// is core/Behind.h's. Only in a copy launched with ELUVIAN_BEHIND=1, which
// game_launch sets: a game the player starts comes forward as ever.
//
// The game imports all three (DAOLib's tools\daoexe.py --imports,
// 2026-10-24); the launch's SW_SHOWNOACTIVATE governs only its first
// ShowWindow, and Windows grants its own SetForegroundWindow whenever no
// window is in front, as a force quit of the copy that was leaves it.

#include "core/Behind.h"
#include "game/GameTools.h"

#include <dao/Hook.h>

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <array>
#include <atomic>

namespace eluvian::game
{
namespace
{

using ForegroundFn = BOOL(WINAPI *)(HWND);
using ShowFn = BOOL(WINAPI *)(HWND, int);
using PositionFn = BOOL(WINAPI *)(HWND, HWND, int, int, int, int, UINT);

struct Kept
{
    dao::Detour<ForegroundFn> foreground;
    dao::Detour<ShowFn> show;
    dao::Detour<PositionFn> position;
    std::atomic<bool> on{false};
    std::atomic<std::uint32_t> refused{0}; // SetForegroundWindow calls refused
    std::atomic<std::uint32_t> shown{0};   // ShowWindow commands made not to activate
    std::atomic<std::uint32_t> placed{0};  // SetWindowPos calls made not to activate or raise
};

Kept &State()
{
    static Kept kept;
    return kept;
}

// The first of each kind is logged, to read what the game asked; the rest
// are counted, for `window`.
void Note(std::atomic<std::uint32_t> &count, const char *what, int asked)
{
    if (count.fetch_add(1) == 0)
    {
        spdlog::info("kept behind: the game's {} ({}) made not to bring it forward; later ones only counted", what,
                     asked);
    }
}

BOOL WINAPI Foreground(HWND /*window*/)
{
    Note(State().refused, "SetForegroundWindow", 0);
    return FALSE; // as Windows answers a request it does not grant
}

BOOL WINAPI Show(HWND window, int command)
{
    const int behind = BehindShowCommand(command);
    if (behind != command)
    {
        Note(State().shown, "ShowWindow", command);
    }
    return State().show.Original()(window, behind);
}

BOOL WINAPI Position(HWND window, HWND after, int x, int y, int width, int height, UINT flags)
{
    const auto behind = static_cast<UINT>(BehindPositionFlags(flags));
    if (behind != flags)
    {
        Note(State().placed, "SetWindowPos", static_cast<int>(flags));
    }
    return State().position.Original()(window, after, x, y, width, height, behind);
}

bool LaunchedBehind()
{
    std::array<wchar_t, 8> value{};
    const DWORD length = GetEnvironmentVariableW(L"ELUVIAN_BEHIND", value.data(), static_cast<DWORD>(value.size()));
    return length == 1 && value[0] == L'1';
}

} // namespace

void KeepBehind()
{
    if (!LaunchedBehind())
    {
        return;
    }
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr)
    {
        spdlog::warn("kept behind: user32.dll is not loaded; the game may come forward");
        return;
    }
    auto &kept = State();
    const bool placed =
        kept.foreground.AttachTo(reinterpret_cast<ForegroundFn>(GetProcAddress(user32, "SetForegroundWindow")),
                                 &Foreground) &&
        kept.show.AttachTo(reinterpret_cast<ShowFn>(GetProcAddress(user32, "ShowWindow")), &Show) &&
        kept.position.AttachTo(reinterpret_cast<PositionFn>(GetProcAddress(user32, "SetWindowPos")), &Position);
    kept.on = placed;
    if (placed)
    {
        spdlog::info("kept behind: launched with ELUVIAN_BEHIND, so the game's own SetForegroundWindow, ShowWindow "
                     "and SetWindowPos do not bring it forward");
    }
    else
    {
        spdlog::warn("kept behind: a detour on user32 was not placed; the game may come forward");
    }
}

std::optional<BehindCounts> KeptBehind()
{
    const auto &kept = State();
    if (!kept.on)
    {
        return std::nullopt;
    }
    return BehindCounts{kept.refused, kept.shown, kept.placed};
}

} // namespace eluvian::game
