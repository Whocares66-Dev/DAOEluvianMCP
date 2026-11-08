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
#include "game/Convert.h"
#include "game/GameTools.h"

#include <dao/Frame.h>
#include <dao/Hook.h>

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <format>
#include <string>

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
    // The front given back where the copy reached it all the same (#179):
    // the window in front when Eluvian loaded, and when that was.
    HWND before = nullptr;
    ULONGLONG loaded = 0;
    std::atomic<std::uint32_t> givenBack{0};
    // The front watched each frame, for the log: whether the game's window
    // had it, how often it came to it, and the last other window that had it
    // (elgara-agent's copy that came forward at a menu, 2026-10-25, with no
    // call of the game's counted).
    bool front = false;
    std::atomic<std::uint32_t> reached{0};
    HWND other = nullptr;
    std::string otherSaid;
    // The user's last input seen (a mouse button, Alt, Win), and whether the
    // front the window holds now is the user's: decided as it arrives, and
    // kept for as long as it holds it.
    ULONGLONG input = 0;
    bool usersFront = false;
    // Once the user brought the game forward themselves, it is theirs for the
    // rest of the run: a window of theirs closing hands the front back to the
    // game they were in, with no input on it (the snipping tool, the user,
    // 2026-10-27), which is theirs too.
    bool usersGame = false;
    // The give-backs of the minute that began at `minute`.
    ULONGLONG minute = 0;
    std::uint32_t thisMinute = 0;
};

Kept &State()
{
    static Kept kept;
    return kept;
}

// Each is logged with its time, the first twenty of a kind, to read what
// the game asked and when; all are counted, for `window`.
constexpr std::uint32_t kMostLogged = 20;

void Note(std::atomic<std::uint32_t> &count, const char *what, int asked)
{
    const std::uint32_t before = count.fetch_add(1);
    if (before < kMostLogged)
    {
        spdlog::info("kept behind: the game's {} ({}) made not to bring it forward, {} ms after load{}", what, asked,
                     GetTickCount64() - State().loaded, before + 1 == kMostLogged ? "; later ones only counted" : "");
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

bool Ours(HWND window)
{
    DWORD process = 0;
    return window != nullptr && GetWindowThreadProcessId(window, &process) != 0 && process == GetCurrentProcessId();
}

// Where the front goes back to: the last other window that had it, else
// the one in front when Eluvian loaded, else, those gone (the copy
// force-quit a moment before), the next visible top-level window in line
// below the game's that is not its own.
// Whether a window is another copy of the game's: the front is never given
// to one, which some agent's test runs in (lib-agent-3's copy got activated
// and closed twice, 2026-10-27, by no one known yet).
bool AGame(HWND window)
{
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (handle == nullptr)
    {
        return false;
    }
    std::array<wchar_t, MAX_PATH> path{};
    auto size = static_cast<DWORD>(path.size());
    const bool read = QueryFullProcessImageNameW(handle, 0, path.data(), &size) != FALSE;
    CloseHandle(handle);
    if (!read)
    {
        return false;
    }
    std::wstring name(path.data(), size);
    name = name.substr(name.find_last_of(L'\\') + 1);
    std::ranges::transform(name, name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return name == L"daorigins.exe";
}

HWND BackTo(HWND front)
{
    const auto &kept = State();
    for (HWND was : {kept.other, kept.before})
    {
        if (was != nullptr && IsWindow(was) != FALSE && IsWindowVisible(was) != FALSE && !Ours(was) && !AGame(was))
        {
            return was;
        }
    }
    for (HWND next = GetWindow(front, GW_HWNDNEXT); next != nullptr; next = GetWindow(next, GW_HWNDNEXT))
    {
        if (IsWindowVisible(next) != FALSE && GetWindow(next, GW_OWNER) == nullptr && !Ours(next) && !AGame(next))
        {
            return next;
        }
    }
    return nullptr;
}

// A window as the log names it: its program, its title.
std::string Said(HWND window)
{
    if (window == nullptr)
    {
        return "no window";
    }
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    std::wstring program(MAX_PATH, L'\0');
    if (HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process); handle != nullptr)
    {
        auto size = static_cast<DWORD>(program.size());
        program.resize(QueryFullProcessImageNameW(handle, 0, program.data(), &size) != FALSE ? size : 0);
        CloseHandle(handle);
        program = program.substr(program.find_last_of(L'\\') + 1);
    }
    else
    {
        program.clear();
    }
    std::wstring title(256, L'\0');
    title.resize(static_cast<std::size_t>(GetWindowTextW(window, title.data(), static_cast<int>(title.size()))));
    return std::format("{} \"{}\" (pid {})", program.empty() ? std::string("a program") : Narrow(program),
                       Narrow(title), process);
}

bool Down(int key)
{
    return (static_cast<std::uint16_t>(GetAsyncKeyState(key)) & 0x8000U) != 0;
}

// Whether the cursor is over the game's own window or the taskbar, where a
// click is the user's asking for the game. A click elsewhere is not: a copy
// came to the front 22 s after its load while the user was clicking in a
// terminal, and was left there as if they had asked for it (elgara-agent,
// 2026-10-27).
bool OverGameOrTaskbar()
{
    POINT at{};
    if (GetCursorPos(&at) == FALSE)
    {
        return false;
    }
    HWND top = GetAncestor(WindowFromPoint(at), GA_ROOT);
    if (Ours(top))
    {
        return true;
    }
    std::array<wchar_t, 32> name{};
    GetClassNameW(top, name.data(), static_cast<int>(name.size()));
    const std::wstring_view kind(name.data());
    return kind == L"Shell_TrayWnd" || kind == L"Shell_SecondaryTrayWnd";
}

// The user's own input, noted each frame: a mouse button on the game's
// window or the taskbar, Alt (Alt-Tab) or Win (Win and a number).
bool UsersInputNow()
{
    for (const int key : {VK_MENU, VK_LWIN, VK_RWIN})
    {
        if (Down(key))
        {
            return true;
        }
    }
    return (Down(VK_LBUTTON) || Down(VK_RBUTTON) || Down(VK_MBUTTON)) && OverGameOrTaskbar();
}

// Each frame: the front watched, its comings and goings logged with the
// window it came from and the keys and buttons held, which tell a user's
// switch (Alt, Win, a click) from one no one asked for.
void WatchFront(HWND front)
{
    auto &kept = State();
    const bool ours = Ours(front);
    if (!ours && front != nullptr && front != kept.other)
    {
        kept.other = front;
        kept.otherSaid = Said(front);
    }
    const ULONGLONG now = GetTickCount64();
    if (UsersInputNow())
    {
        kept.input = now;
    }
    if (ours && !kept.front)
    {
        kept.reached.fetch_add(1);
        if (now - kept.minute >= 60'000)
        {
            kept.minute = now;
            kept.thisMinute = 0;
        }
        kept.usersFront =
            kept.usersGame || !BehindGivesFrontBack(kept.input == 0 ? now : now - kept.input, kept.thisMinute);
        // Brought by the user's input: what the rule says with no give-backs spent.
        kept.usersGame = kept.usersGame || (kept.input != 0 && !BehindGivesFrontBack(now - kept.input, 0));
        spdlog::info("kept behind: the game's window came to the front {} ms after load, from {}{}; held: alt {}, "
                     "win {}, mouse {} (on the game or taskbar {}); the game's calls so far: SetForegroundWindow {}, "
                     "ShowWindow {}, "
                     "SetWindowPos {}; {}",
                     GetTickCount64() - kept.loaded, kept.otherSaid.empty() ? "no window" : kept.otherSaid,
                     kept.other != nullptr && IsWindow(kept.other) == FALSE ? ", which is gone" : "", Down(VK_MENU),
                     Down(VK_LWIN) || Down(VK_RWIN), Down(VK_LBUTTON) || Down(VK_RBUTTON), OverGameOrTaskbar(),
                     kept.refused.load(), kept.shown.load(), kept.placed.load(),
                     kept.usersFront ? "left there: the user's own (their input just come, or the game theirs since "
                                       "they took it), or the minute's give-backs spent"
                                     : "to be given back");
    }
    else if (!ours && kept.front)
    {
        spdlog::info("kept behind: the game's window left the front {} ms after load, to {}",
                     GetTickCount64() - kept.loaded, front == nullptr ? std::string("no window") : kept.otherSaid);
    }
    kept.front = ours;
}

// Each frame: a copy kept behind that is in front all the same gives the
// front back, with the original calls its detours stand in front of; in
// front, the process may set the foreground (core/Behind.h says when).
void GiveFrontBack()
{
    auto &kept = State();
    if (!kept.on)
    {
        return;
    }
    HWND front = GetForegroundWindow();
    WatchFront(front);
    if (!Ours(front) || kept.usersFront)
    {
        return;
    }
    HWND to = BackTo(front);
    if (to == nullptr)
    {
        // Said once, and left in front: a copy came forward as the EA
        // launcher's "Preparing game..." closed, judged to be given back, and
        // its log said no more (the user's preview, 2026-10-30).
        spdlog::info("kept behind: no window to give the front back to {} ms after load (the one it came from "
                     "gone, none visible below it that is not a copy of the game's); left in front",
                     GetTickCount64() - kept.loaded);
        kept.usersFront = true;
        return;
    }
    kept.position.Original()(front, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const BOOL given = kept.foreground.Original()(to);
    kept.givenBack.fetch_add(1);
    kept.thisMinute += 1;
    kept.usersFront = kept.thisMinute >= 5; // a refused give-back tried again, the minute's few at most
    spdlog::info("kept behind: the front given back {} ms after load to {} ({})", GetTickCount64() - kept.loaded,
                 Said(to), given != FALSE ? "taken" : "refused");
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
    kept.before = GetForegroundWindow();
    kept.loaded = GetTickCount64();
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
        dao::OnFrame([] { GiveFrontBack(); });
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
    return BehindCounts{kept.refused, kept.shown, kept.placed, kept.givenBack, kept.reached};
}

} // namespace eluvian::game
