// `peek`: a field of a live object read by a chain of offsets, apart from the
// game so a test holds the chain and the decoding (#161). Some of the engine's
// rules hinge on a value at an offset of a live object, such as a cutscene
// actor's height, +0x64 of an object the creature holds at +0x7B0 or +0x1F0
// (0x007CE5A0, lib-agent-2's reading, 2026-10-19), which no tool read: each
// check took a plugin of its own. src/game's `peek` reads the game's memory
// through DAOLib's guarded copy, so a bad address is an answer, not a crash.
#pragma once

#include "core/Tool.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace eluvian
{

// The memory a peek reads: `read` copies `size` bytes at `address` into `out`,
// false if any byte cannot be read; `className` names the object at an
// address by its run-time type, empty for none.
struct PeekMemory
{
    std::function<bool(std::uint32_t address, void *out, std::size_t size)> read;
    std::function<std::string(std::uint32_t address)> className;
};

// The types a peek reads a value as.
inline constexpr std::string_view kPeekTypes[] = {"f32", "i32",  "u32",  "u16",     "u8",
                                                  "ptr", "vec3", "quat", "wstring", "bytes"};

// The offsets a call gives: numbers, or texts in hex ("0x7B0", "-0x10") or
// decimal. A ToolError (peek's) for anything else.
std::vector<std::int64_t> PeekOffsets(const Json &offsets);

// Reads from `base` along `offsets`: every offset but the last is a pointer
// read at that offset from where the chain is, and followed; the value is read
// at the last, as `type`, `count` of them (`bytes`: `count` bytes). No offsets
// read at `base` itself. Answers each step taken (where it read, the pointer
// and its class), the value's address and the value; a read that fails ends
// the answer there with `readable: false` and where.
Json Peek(const PeekMemory &memory, std::uint32_t base, const std::vector<std::int64_t> &offsets, std::string_view type,
          std::size_t count);

// An address as answers write it: "0x007ce5a0".
std::string AddressText(std::uint32_t address);

} // namespace eluvian
