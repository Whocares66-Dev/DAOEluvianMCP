// The call trace's stub (#197), run: written into executable memory as the
// game's side writes it, and called through as a detoured function is, with
// no detour, since the recorder sends each call on to where it says.
#include "core/CallTrace.h"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <cstddef>
#include <cstring>

using namespace eluvian::calltrace;

namespace
{

Frame g_seen{};
int g_records = 0;
std::uint32_t g_onward = 0;

void __cdecl Recorder(Frame *frame)
{
    // Only the words each test's calls pass: past them is the caller's own
    // frame, which ASan guards (the game's recorder reads them checked).
    std::memcpy(&g_seen, frame, offsetof(Frame, words) + (2 * sizeof(std::uint32_t)));
    ++g_records;
    frame->slot = g_onward;
}

// thiscall's shape: `self` in ecx, `delay` and `more` on the stack, popped by
// the function.
int __fastcall Traced(int self, int /*edx*/, int delay, int more)
{
    return self + (delay * 10) + (more * 100);
}

// stdcall, two words on the stack, popped by the function.
int __stdcall TracedStd(int a, int b)
{
    return a - b;
}

struct Code
{
    std::uint8_t *page = nullptr;
    std::uint32_t common = 0;

    Code()
    {
        page =
            static_cast<std::uint8_t *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        REQUIRE(page != nullptr);
        common = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(page));
        const auto stub = CommonStub(common, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&Recorder)));
        REQUIRE(stub.size() == kStubSize);
        std::memcpy(page, stub.data(), stub.size());
    }
    ~Code()
    {
        VirtualFree(page, 0, MEM_RELEASE);
    }
    Code(const Code &) = delete;
    Code &operator=(const Code &) = delete;

    // The thunk for `slot`, its address.
    std::uintptr_t Thunk(std::uint32_t slot) const
    {
        const std::size_t offset = 64 + (slot * 16);
        const auto at = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(page + offset));
        const auto thunk = SlotThunk(at, slot, common);
        REQUIRE(thunk.size() == kThunkSize);
        std::memcpy(page + offset, thunk.data(), thunk.size());
        return at;
    }
};

} // namespace

TEST_CASE("a traced thiscall reaches its function as called, and the recorder sees this, its arguments and its caller",
          "[call_trace]")
{
    Code code;
    g_records = 0;
    g_onward = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&Traced));
    using Fn = int(__fastcall *)(int, int, int, int);
    const auto through = reinterpret_cast<Fn>(code.Thunk(3));

    const int answer = through(7, 0, 5, 2);

    CHECK(answer == 7 + 50 + 200);
    CHECK(g_records == 1);
    CHECK(g_seen.slot == 3);
    CHECK(g_seen.ecx == 7);
    CHECK(g_seen.words[0] == 5);
    CHECK(g_seen.words[1] == 2);
    CHECK(g_seen.ret != 0);
}

TEST_CASE("a traced stdcall keeps its stack", "[call_trace]")
{
    Code code;
    g_records = 0;
    g_onward = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&TracedStd));
    using Fn = int(__stdcall *)(int, int);
    const auto through = reinterpret_cast<Fn>(code.Thunk(15));

    // Twice: a stack left off by the first would show in the second.
    CHECK(through(9, 4) == 5);
    CHECK(through(40, 2) == 38);
    CHECK(g_records == 2);
    CHECK(g_seen.slot == 15);
    CHECK(g_seen.words[0] == 40);
    CHECK(g_seen.words[1] == 2);
}

TEST_CASE("the stub's and a thunk's bytes", "[call_trace]")
{
    const auto stub = CommonStub(0x1000, 0x2000);
    CHECK(stub == std::vector<std::uint8_t>{0x9C, 0x60, 0x54, 0xE8, 0xF8, 0x0F, 0x00, 0x00, 0x83, 0xC4, 0x04, 0x61,
                                            0x9D, 0xC3});
    const auto thunk = SlotThunk(0x1040, 2, 0x1000);
    CHECK(thunk == std::vector<std::uint8_t>{0x68, 0x02, 0x00, 0x00, 0x00, 0xE9, 0xB6, 0xFF, 0xFF, 0xFF});
}

TEST_CASE("an address as an agent writes it", "[call_trace]")
{
    CHECK(ParseAddress("0x0069F360") == 0x0069F360u);
    CHECK(ParseAddress("0X6bc670") == 0x006BC670u);
    CHECK(ParseAddress("6943584") == 0x0069F360u);
    CHECK_FALSE(ParseAddress(""));
    CHECK_FALSE(ParseAddress("0x"));
    CHECK_FALSE(ParseAddress("0"));
    CHECK_FALSE(ParseAddress("0x69F360 "));
    CHECK_FALSE(ParseAddress("SetDecayDelay"));
    CHECK_FALSE(ParseAddress("0x100000000"));
}
