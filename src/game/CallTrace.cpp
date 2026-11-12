// `call_trace` and `call_trace_read` (#197): who calls a function of the
// exe's, and with what, recorded as the calls come, on any thread, across
// loads.
//
// Asked by paragon-agent-2 for DAOParagonEngine #428: two dead soldiers
// are destroyed during a load from the main menu and kept by a load while
// their area is current, and neither the engine's path nor the save's data
// nor the scripts said why; who calls CCCreature::SetDecayDelay
// (0x0069F360) and the corpse's destroy during the load is what is left.
//
// Each traced function is detoured to a thunk of its slot, which jumps to
// one stub that saves every register and the flags, calls Record with them,
// and goes on to the trampoline Record names (core/CallTrace.h). So the hook
// is the same for any calling convention and changes nothing. A detour
// stays to the game's end: one taken out frees its trampoline, where a
// loading thread could be. Arming again records the new set only.
//
// Record runs on whichever thread calls, the loading thread among them,
// while the frame thread may be held: it takes only its own lock, reads
// `this` through checked reads, and allocates within the room reserved at
// arming. The read is a listener tool, so it answers during a load.
#include "core/CallTrace.h"
#include "game/GameTools.h"
#include "game/Health.h"
#include "game/Shared.h"

#include <dao/Module.h>
#include <dao/SafeRead.h>
#include <dao/TypeName.h>
#include <dao/core/Rtti.h>

#include <Windows.h>

#include <detours/detours.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace eluvian::game
{

namespace
{

namespace ct = calltrace;

// One call as Record saw it.
struct Call
{
    std::uint32_t slot = 0;
    std::uint64_t frame = 0;
    std::int32_t mode = -1;
    DWORD thread = 0;
    double ms = 0;
    std::uint32_t ecx = 0;
    std::uint32_t edx = 0;
    std::uint32_t eax = 0;
    std::string_view thisClass; // decorated, in the exe's RTTI, which lives as long as the game
    std::optional<ObjectId> thisId;
    std::uint32_t ret = 0;
    std::array<std::uint32_t, ct::kMostWords> words{};
};

// A traced function's slot. `onward` is the trampoline, which Detours writes
// during its commit just after the jump to the thunk: Record waits for it.
struct Slot
{
    std::uint32_t target = 0; // runtime address; 0 while the slot is free
    PVOID onward = nullptr;
    std::atomic<bool> recording{false};
};

// Never destroyed: a thread can be in a stub when the game exits.
struct State
{
    std::mutex lock; // over the fields below `slots`
    std::array<Slot, ct::kMostTraced> slots;
    std::uint8_t *code = nullptr; // the stub, then each slot's thunk
    const Watch *watch = nullptr;
    DWORD frameThread = 0;
    std::chrono::steady_clock::time_point start;
    std::size_t words = 2;
    std::size_t most = 2000;
    std::vector<Call> calls;
    std::uint64_t dropped = 0;
    std::uint64_t armings = 0;
};

State &Trace()
{
    static auto *state = new State; // NOLINT(cppcoreguidelines-owning-memory): kept to the game's end
    return *state;
}

std::uint32_t Address(const void *pointer)
{
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

void Keep(State &state, std::uint32_t slot, const ct::Frame &frame) noexcept
{
    try
    {
        Call call;
        call.slot = slot;
        call.frame = state.watch->Frames();
        call.mode = state.watch->Mode();
        call.thread = GetCurrentThreadId();
        call.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - state.start).count();
        call.ecx = frame.ecx;
        call.edx = frame.edx;
        call.eax = frame.eax;
        call.ret = frame.ret;
        call.thisClass = dao::DecoratedTypeName(reinterpret_cast<const void *>(static_cast<std::uintptr_t>(frame.ecx)));
        if (!call.thisClass.empty())
        {
            // CGameObject's id at +0x10 (dao/Objects.h); checked, since
            // `this` can be anything.
            call.thisId = dao::TryRead<ObjectId>(static_cast<std::uintptr_t>(frame.ecx) + 0x10);
        }
        const std::lock_guard hold(state.lock);
        // Checked: past a call's own arguments is its caller's frame, or
        // the stack's end.
        if (!dao::TryCopy(call.words.data(), reinterpret_cast<std::uintptr_t>(frame.words),
                          state.words * sizeof(std::uint32_t)))
        {
            call.words.fill(0);
        }
        if (state.calls.size() < state.most)
        {
            state.calls.push_back(call);
        }
        else
        {
            ++state.dropped;
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch): nothing may leave a hook on the game's thread
    {
    }
}

// The stub's recorder, on the calling thread.
void __cdecl Record(ct::Frame *frame)
{
    State &state = Trace();
    const std::uint32_t slot = frame->slot;
    Slot &traced = state.slots[slot];
    if (traced.recording.load())
    {
        Keep(state, slot, *frame);
    }
    PVOID onward = nullptr;
    while ((onward = std::atomic_ref(traced.onward).load()) == nullptr)
    {
        YieldProcessor();
    }
    frame->slot = Address(onward);
}

// The exe's code section, runtime addresses.
std::pair<std::uint32_t, std::uint32_t> CodeSection()
{
    const auto base = dao::Module::Base();
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0)
        {
            const auto start = static_cast<std::uint32_t>(base + section->VirtualAddress);
            return {start, start + section->Misc.VirtualSize};
        }
    }
    return {0, 0};
}

// The stub and the thunks, written once.
void WriteCode(State &state)
{
    if (state.code != nullptr)
    {
        return;
    }
    auto *page =
        static_cast<std::uint8_t *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (page == nullptr)
    {
        throw ToolError("call_trace: no memory for the stub");
    }
    const auto stub = ct::CommonStub(Address(page), Address(reinterpret_cast<const void *>(&Record)));
    std::memcpy(page, stub.data(), stub.size());
    for (std::uint32_t slot = 0; slot < ct::kMostTraced; ++slot)
    {
        std::uint8_t *at = page + 16 + (slot * 16);
        const auto thunk = ct::SlotThunk(Address(at), slot, Address(page));
        std::memcpy(at, thunk.data(), thunk.size());
    }
    DWORD was = 0;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &was);
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    state.code = page;
}

void *ThunkOf(const State &state, std::size_t slot)
{
    return state.code + 16 + (slot * 16);
}

// The slot tracing `target`, detouring it into a free one the first time.
std::size_t SlotFor(State &state, std::uint32_t target, std::string_view written)
{
    for (std::size_t i = 0; i < ct::kMostTraced; ++i)
    {
        if (state.slots[i].target == target)
        {
            return i;
        }
    }
    for (std::size_t i = 0; i < ct::kMostTraced; ++i)
    {
        Slot &slot = state.slots[i];
        if (slot.target != 0)
        {
            continue;
        }
        slot.onward = reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(target));
        // Detours writes the trampoline into `onward` itself; it is read by
        // Record through atomic_ref, never cached.
        if (DetourTransactionBegin() != NO_ERROR)
        {
            throw ToolError("call_trace: Detours would not begin");
        }
        DetourUpdateThread(GetCurrentThread());
        if (DetourAttach(&slot.onward, ThunkOf(state, i)) != NO_ERROR || DetourTransactionCommit() != NO_ERROR)
        {
            DetourTransactionAbort();
            slot.onward = nullptr;
            throw ToolError(std::format("call_trace: Detours refused {}: it may not be a function's start", written));
        }
        slot.target = target;
        return i;
    }
    throw ToolError(std::format("call_trace: {} functions are traced already in this run of the game, the most; a "
                                "detour stays to the game's end",
                                ct::kMostTraced));
}

std::string Hex(std::uint32_t value)
{
    return std::format("0x{:08X}", value);
}

// A code address as the GOG build's, and the address library's function it
// is in, by ID and offset, where it knows one.
Json CodeJson(std::uint32_t runtime)
{
    Json answer = {{"address", Hex(dao::Module::Preferred(runtime))}};
    if (const auto place = dao::Module::Containing(runtime))
    {
        answer["function"] = {{"id", static_cast<std::uint32_t>(place->id)}, {"offset", place->offset}};
    }
    return answer;
}

ToolResult Arm(const Watch &watch, const Json &raw)
{
    const Args args(raw, "call_trace");
    std::vector<std::pair<std::uint32_t, std::string>> asked;
    if (args.Has("addresses"))
    {
        const Json &list = raw.at("addresses");
        if (!list.is_array())
        {
            throw ToolError("call_trace: addresses is a list of addresses, as \"0x0069F360\"");
        }
        for (const Json &item : list)
        {
            const std::string written = item.is_string() ? item.get<std::string>() : item.dump();
            const auto address = ct::ParseAddress(written);
            if (!address)
            {
                throw ToolError(std::format("call_trace: {} is not an address; write it as \"0x0069F360\"", written));
            }
            asked.emplace_back(*address, written);
        }
    }
    if (asked.size() > ct::kMostTraced)
    {
        throw ToolError(std::format("call_trace: {} addresses at most", ct::kMostTraced));
    }
    const auto words = static_cast<std::size_t>(args.Integer("words", 2, 0, ct::kMostWords));
    const auto most = static_cast<std::size_t>(args.Integer("most", 2000, 1, 20000));

    if (!dao::Module::IsKnownExecutable())
    {
        throw ToolError("call_trace: the running exe is not a build the address library knows");
    }
    const auto [codeStart, codeEnd] = CodeSection();
    const auto base = dao::Module::Base();
    std::vector<std::uint32_t> targets;
    for (const auto &[address, written] : asked)
    {
        const auto runtime = static_cast<std::uint32_t>(address - dao::core::kPreferredBase + base);
        if (runtime < codeStart || runtime + 5 > codeEnd)
        {
            throw ToolError(std::format("call_trace: {} is not in the exe's code ({} to {})", written,
                                        Hex(codeStart - static_cast<std::uint32_t>(base) + dao::core::kPreferredBase),
                                        Hex(codeEnd - static_cast<std::uint32_t>(base) + dao::core::kPreferredBase)));
        }
        targets.push_back(runtime);
    }

    State &state = Trace();
    WriteCode(state);
    for (Slot &slot : state.slots)
    {
        slot.recording = false;
    }
    {
        const std::lock_guard hold(state.lock);
        state.calls.clear();
        state.calls.reserve(most);
        state.dropped = 0;
        state.words = words;
        state.most = most;
        state.watch = &watch;
        state.frameThread = GetCurrentThreadId();
        state.start = std::chrono::steady_clock::now();
        ++state.armings;
    }
    Json traced = Json::array();
    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        const std::size_t slot = SlotFor(state, targets[i], asked[i].second);
        state.slots[slot].recording = true;
        Json one = CodeJson(targets[i]);
        one["slot"] = slot;
        traced.push_back(std::move(one));
    }
    std::size_t detoured = 0;
    for (const Slot &slot : state.slots)
    {
        detoured += slot.target != 0 ? 1 : 0;
    }
    return Json{{"traced", std::move(traced)},
                {"words", words},
                {"most", most},
                {"detoured", detoured},
                {"room", ct::kMostTraced - detoured},
                {"mode", ModeName(watch.Mode())},
                {"frame", watch.Frames()}};
}

Json CallJson(const State &state, const Call &call)
{
    Json words = Json::array();
    for (std::size_t i = 0; i < state.words; ++i)
    {
        words.push_back(Hex(call.words[i]));
    }
    Json self = {{"ecx", Hex(call.ecx)}};
    if (!call.thisClass.empty())
    {
        // From the name kept at the call: the object may be gone by now.
        self["class"] = dao::core::UndecorateClassName(call.thisClass);
        self["id"] = call.thisId ? Json(FormatId(*call.thisId)) : Json(nullptr);
    }
    return Json{{"function", Hex(dao::Module::Preferred(state.slots[call.slot].target))},
                {"frame", call.frame},
                {"ms", call.ms},
                {"mode", ModeName(call.mode)},
                {"thread", call.thread},
                {"frameThread", call.thread == state.frameThread},
                {"this", std::move(self)},
                {"words", std::move(words)},
                {"edx", Hex(call.edx)},
                {"eax", Hex(call.eax)},
                {"caller", CodeJson(call.ret)}};
}

ToolResult ReadCalls(const Json &raw)
{
    const Args args(raw, "call_trace_read");
    State &state = Trace();
    const std::lock_guard hold(state.lock);
    const auto since = static_cast<std::size_t>(args.Integer("since", 0, 0, 1'000'000));
    const auto count = static_cast<std::size_t>(args.Integer("count", 200, 1, 2000));
    if (state.armings == 0)
    {
        return Json{{"armed", false}, {"note", "nothing was armed in this run of the game: call call_trace first"}};
    }
    Json calls = Json::array();
    std::size_t at = since;
    for (; at < state.calls.size() && calls.size() < count; ++at)
    {
        calls.push_back(CallJson(state, state.calls[at]));
    }
    std::map<std::string, std::size_t> perFunction;
    for (const Call &call : state.calls)
    {
        ++perFunction[Hex(dao::Module::Preferred(state.slots[call.slot].target))];
    }
    return Json{{"recorded", state.calls.size()},
                {"dropped", state.dropped},
                {"perFunction", perFunction},
                {"since", since},
                {"next", at},
                {"calls", std::move(calls)}};
}

} // namespace

void AddCallTrace(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        Spec("call_trace",
             "Traces calls to functions of the game's exe: from now on each call to one of `addresses` is "
             "recorded, on whatever thread makes it, through loads and the main menu, until the next call_trace; "
             "read them with call_trace_read. Use it to see who calls an engine function and with what, where "
             "reading the code does not settle it. Pass each function's start as the address library and Ghidra "
             "give it for GOG's build (\"0x0069F360\"); an address that is not a function's start can crash the "
             "game. Calling again starts a new record of the addresses given (an empty list records nothing); "
             "a function once traced stays detoured until the game ends, harmless when not recording, and at "
             "most 16 are, `room` saying how many more. The hook saves and restores every register, so it "
             "works for any calling convention and changes nothing the function does. It can be armed at the "
             "main menu, before a load. Answers each address's `slot` and the address library's function it "
             "is in (`function`: the ID and offset; offset 0 is that function's start).",
             Json{{"addresses",
                   {{"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Function starts in the exe, as \"0x0069F360\". Empty or absent: stop "
                                    "recording."}}},
                  {"words",
                   {{"type", "integer"},
                    {"description", "Stack words to keep past the return address, 0 to 8: the call's stack "
                                    "arguments, the first for a thiscall. Default 2."}}},
                  {"most", {{"type", "integer"}, {"description", "Calls to keep at most, 1 to 20000. Default 2000."}}}},
             {}),
        [&watch](const Json &raw) -> ToolResult { return Arm(watch, raw); });

    bench.Tools().Add(
        Read(
            "call_trace_read",
            "Reads the calls call_trace has recorded since it was last called, oldest first; it answers during "
            "a load, when other tools wait. Each call has its `function`, the `frame` and `ms` since arming, "
            "the game `mode` then, the `thread` and whether it was the frame thread (`frameThread`; a load "
            "runs on another), `this` (ecx, with its `class` and object `id` where it points at a game object), "
            "`words` (the stack past the return address: a call's stack arguments, the first of a thiscall "
            "first), `edx` and `eax`, and its `caller`: the return address as GOG's build has it and the "
            "address library's function it is in (`function`: ID and offset; `python tools\\address_ids.py "
            "<id>` in DAOLib names it). `recorded` counts them all, `perFunction` by function, `dropped` those "
            "past `most`; page with `since` and `next`.",
            Json{
                {"since", {{"type", "integer"}, {"description", "The first call to answer, by its place. Default 0."}}},
                {"count", {{"type", "integer"}, {"description", "Calls to answer at most, 1 to 2000. Default 200."}}}},
            {}, true, Thread::Listener),
        [](const Json &raw) -> ToolResult { return ReadCalls(raw); });
}

} // namespace eluvian::game
