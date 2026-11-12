// The call trace (#197): which code calls a function of the exe's, and with
// what, recorded from the stub a traced call passes through on its way to
// the function. The stub's code and the frame it hands the recorder are
// decided here, as bytes and offsets, so they are tested without the game;
// src/game/CallTrace.cpp writes them into the process and detours the
// functions to them.
//
// The stub knows no calling convention: it saves every register and the
// flags, hands the recorder a pointer to them and to the stack as the call
// left it, restores them and goes on to the function as the call would
// have. So it changes nothing for any function, thiscall, cdecl, stdcall or
// the compiler's own. Asked by paragon-agent-2 to see who calls
// CCCreature::SetDecayDelay and a corpse's destroy during a load
// (DAOParagonEngine #428).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace eluvian::calltrace
{

// Functions traced at most in one run of the game: each keeps its detour to
// the game's end, since a thread can be inside its trampoline when it would
// be taken out.
inline constexpr std::size_t kMostTraced = 16;

// Stack words past the return address the recorder may read: the call's
// first arguments, for a convention that passes them on the stack.
inline constexpr std::size_t kMostWords = 8;

// What the stub hands the recorder, lowest address first: pushad's eight
// registers, the flags, the slot the thunk pushed, the caller's return
// address, then the stack as the call left it. The recorder writes where to
// go on over `slot`; the stub's `ret` takes it there, the stack as the call
// left it.
struct Frame
{
    std::uint32_t edi;
    std::uint32_t esi;
    std::uint32_t ebp;
    std::uint32_t esp; // pushad's: the stack before it, not the call's
    std::uint32_t ebx;
    std::uint32_t edx;
    std::uint32_t ecx;
    std::uint32_t eax;
    std::uint32_t eflags;
    std::uint32_t slot; // in: the traced function's slot; out: where the call goes on
    std::uint32_t ret;  // the caller's return address
    std::uint32_t words[kMostWords];
};

// The stub every slot's thunk jumps to, written at `at`, calling the
// recorder, a cdecl function taking a Frame*, at `record`:
// pushfd; pushad; push esp; call record; add esp, 4; popad; popfd; ret.
std::vector<std::uint8_t> CommonStub(std::uint32_t at, std::uint32_t record);

// A slot's thunk, written at `at`, which a traced function's detour runs:
// push slot; jmp common.
std::vector<std::uint8_t> SlotThunk(std::uint32_t at, std::uint32_t slot, std::uint32_t common);

// Bytes a thunk takes, and the stub.
inline constexpr std::size_t kThunkSize = 10;
inline constexpr std::size_t kStubSize = 14;

// An address as an agent writes it: hex with 0x, as Ghidra and the
// address library print them, or decimal. Nullopt for anything else, zero,
// or past 32 bits.
std::optional<std::uint32_t> ParseAddress(std::string_view text);

} // namespace eluvian::calltrace
