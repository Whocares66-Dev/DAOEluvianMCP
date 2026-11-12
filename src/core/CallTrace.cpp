// The call trace's stub code and addresses (CallTrace.h).
#include "core/CallTrace.h"

#include <charconv>

namespace eluvian::calltrace
{

static_assert(offsetof(Frame, eflags) == 32, "pushad pushes eight registers under the flags");
static_assert(offsetof(Frame, slot) == 36, "the thunk's push is under the flags");
static_assert(offsetof(Frame, ret) == 40, "the return address is under the thunk's push");

namespace
{

void Word(std::vector<std::uint8_t> &code, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
    {
        code.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

// A call's or a jmp's rel32: from the end of the instruction.
std::uint32_t Relative(std::uint32_t from, std::uint32_t to)
{
    return to - from;
}

} // namespace

std::vector<std::uint8_t> CommonStub(std::uint32_t at, std::uint32_t record)
{
    std::vector<std::uint8_t> code = {0x9C, 0x60, 0x54, 0xE8}; // pushfd; pushad; push esp; call
    Word(code, Relative(at + 8, record));
    code.insert(code.end(), {0x83, 0xC4, 0x04, 0x61, 0x9D, 0xC3}); // add esp, 4; popad; popfd; ret
    return code;
}

std::vector<std::uint8_t> SlotThunk(std::uint32_t at, std::uint32_t slot, std::uint32_t common)
{
    std::vector<std::uint8_t> code = {0x68}; // push imm32
    Word(code, slot);
    code.push_back(0xE9); // jmp rel32
    Word(code, Relative(at + kThunkSize, common));
    return code;
}

std::optional<std::uint32_t> ParseAddress(std::string_view text)
{
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        text.remove_prefix(2);
        base = 16;
    }
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value == 0)
    {
        return std::nullopt;
    }
    return value;
}

} // namespace eluvian::calltrace
