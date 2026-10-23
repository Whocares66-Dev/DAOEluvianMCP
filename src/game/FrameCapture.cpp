// `frame_capture`: one frame's draws, taken through DAOLib's watch on the
// device (dao::render, DAOLib #246) and copied into core/FrameCapture.h's
// records, which decide the answer (#160).
//
// The watch is placed at load (WatchDraws), before the game makes its
// device: a device made before it is not watched. The draws come on the
// render thread; the call waits for the armed frame's present on the
// listener's, so the frame thread is never held, and a frame the game does
// not draw (a minimised fullscreen game, #148) is a time run out, not a
// stall.
#include "core/FrameCapture.h"
#include "game/Capture.h"
#include "game/GameTools.h"
#include "game/Shared.h"

#include <dao/Render.h>
#include <dao/core/Ctab.h>

#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <vector>

namespace eluvian::game
{

namespace
{

namespace render = dao::render;
namespace ctab = dao::core::ctab;

// What the watch's callbacks and the call share, under `lock`.
struct Capture
{
    std::mutex lock;
    std::condition_variable presented;
    std::optional<render::Watcher> watcher;
    bool collecting = false; // a call armed the watch and waits
    bool done = false;       // the armed frame was presented
    FrameCaptured frame;
    // Each shader's id, hashed once a capture and not at each of its draws
    // (about 2,120 a frame in Redcliffe castle, lib-agent, 2026-10-23). Not by
    // the bytecode's place: DAOLib reads every shader into the one buffer
    // (Render.cpp's t_scratch), so it is looked up by its size and ends and
    // then compared whole, which is far cheaper than hashing it all again.
    struct Known
    {
        std::vector<std::byte> bytecode;
        std::string id;
    };
    std::map<std::pair<std::size_t, std::uint64_t>, std::vector<Known>> ids;
};

// A shader's ends, as a key to look its id up by: the first and last 64
// bytes, where its version, table and last instructions are.
std::uint64_t Ends(std::span<const std::byte> bytecode)
{
    constexpr std::size_t kEnd = 64;
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix = [&hash](std::span<const std::byte> part) {
        for (const std::byte b : part)
        {
            hash ^= static_cast<std::uint64_t>(b);
            hash *= 0x100000001b3ULL;
        }
    };
    const std::size_t end = std::min(kEnd, bytecode.size());
    mix(bytecode.first(end));
    mix(bytecode.last(end));
    return hash;
}

// A shader's id, from the capture's own if it was met before. Under the
// capture's lock.
const std::string &IdOf(Capture &capture, std::span<const std::byte> bytecode)
{
    auto &same = capture.ids[{bytecode.size(), Ends(bytecode)}];
    for (const auto &known : same)
    {
        if (std::ranges::equal(known.bytecode, bytecode))
        {
            return known.id;
        }
    }
    same.push_back(Capture::Known{{bytecode.begin(), bytecode.end()}, ShaderId(bytecode)});
    return same.back().id;
}

Capture &Captured()
{
    static Capture capture;
    return capture;
}

const char *SetName(ctab::RegisterSet set)
{
    switch (set)
    {
    case ctab::RegisterSet::Bool:
        return "bool";
    case ctab::RegisterSet::Int4:
        return "int4";
    case ctab::RegisterSet::Float4:
        return "float4";
    case ctab::RegisterSet::Sampler:
        return "sampler";
    }
    return "?";
}

const char *CallName(render::Call call)
{
    switch (call)
    {
    case render::Call::Primitive:
        return "DrawPrimitive";
    case render::Call::IndexedPrimitive:
        return "DrawIndexedPrimitive";
    case render::Call::PrimitiveUp:
        return "DrawPrimitiveUP";
    case render::Call::IndexedPrimitiveUp:
        return "DrawIndexedPrimitiveUP";
    }
    return "?";
}

// A shader as the records keep it: everything copied, since the watch's
// views last the callback alone. Under the capture's lock.
CapturedShader Copy(Capture &capture, const render::Shader &shader)
{
    CapturedShader out;
    if (shader.bytecode.empty())
    {
        return out;
    }
    out.id = IdOf(capture, shader.bytecode);
    out.bytes = shader.bytecode.size();
    if (shader.table != nullptr)
    {
        out.target = shader.table->target;
    }
    out.constants.reserve(shader.values.size());
    for (const auto &value : shader.values)
    {
        if (value.constant == nullptr)
        {
            continue;
        }
        const ctab::Constant &constant = *value.constant;
        out.constants.push_back(CapturedConstant{constant.name, SetName(constant.set), constant.index, constant.count,
                                                 value.floats, value.ints, value.bools, constant.rows, constant.columns,
                                                 constant.kind == ctab::Class::MatrixColumns});
    }
    for (const auto &sampled : shader.textures)
    {
        const ctab::Constant *sampler = shader.table != nullptr ? ctab::At(*shader.table, ctab::RegisterSet::Sampler,
                                                                           static_cast<std::uint16_t>(sampled.sampler))
                                                                : nullptr;
        out.textures.push_back(
            CapturedTexture{sampled.sampler, sampler != nullptr ? sampler->name : "", Narrow(sampled.name)});
    }
    return out;
}

// On the render thread, for each draw of an armed frame.
void OnDraw(const render::Draw &draw)
{
    auto &capture = Captured();
    const std::lock_guard hold(capture.lock);
    if (!capture.collecting)
    {
        return;
    }
    CapturedDraw out;
    out.index = draw.index;
    out.call = CallName(draw.call);
    out.type = draw.type;
    out.baseVertex = draw.baseVertex;
    out.minIndex = draw.minIndex;
    out.vertices = draw.vertices;
    out.start = draw.start;
    out.primitives = draw.primitives;
    out.vertex = Copy(capture, draw.vertex);
    out.pixel = Copy(capture, draw.pixel);
    out.states.reserve(draw.states.size());
    for (const auto &state : draw.states)
    {
        out.states.emplace_back(state.state, state.value);
    }
    out.material = Narrow(draw.material); // the names are views for this call only
    out.node = draw.node;
    out.object = draw.object;
    out.model = Narrow(draw.model);
    out.mesh = Narrow(draw.mesh);
    capture.frame.draws.push_back(std::move(out));
}

// On the render thread, at the armed frame's present: its draws are all in.
void OnPresent(std::uint64_t frame)
{
    auto &capture = Captured();
    {
        const std::lock_guard hold(capture.lock);
        if (!capture.collecting)
        {
            return;
        }
        capture.frame.frame = frame;
        capture.collecting = false;
        capture.done = true;
    }
    capture.presented.notify_all();
}

// Takes the next frame whole, or throws ToolError saying why there was none.
FrameCaptured TakeFrame(std::vector<std::uint32_t> states, std::chrono::milliseconds wait)
{
    auto &capture = Captured();
    std::unique_lock hold(capture.lock);
    if (!capture.watcher)
    {
        throw ToolError("frame_capture: the draws are not watched in this process; Eluvian places DAOLib's "
                        "watch on them when the game loads it");
    }
    if (capture.collecting)
    {
        throw ToolError("frame_capture: another capture is waiting for its frame; call again when it has answered");
    }
    capture.collecting = true;
    capture.done = false;
    capture.frame = {};
    capture.ids.clear();
    capture.watcher->Arm(1, std::move(states));
    if (capture.presented.wait_for(hold, wait, [&capture] { return capture.done; }))
    {
        return std::move(capture.frame);
    }
    capture.watcher->Disarm();
    capture.collecting = false;
    hold.unlock();
    if (GameWindowPlace().minimised)
    {
        throw ToolError(
            std::format("frame_capture: no frame was drawn within {} ms, and {}", wait.count(), kMinimised));
    }
    if (render::Frame() == 0)
    {
        throw ToolError(std::format("frame_capture: no frame was drawn within {} ms, and none has been since the "
                                    "device was made: the game's device was made before the watch, which then "
                                    "sees none of its draws",
                                    wait.count()));
    }
    throw ToolError(std::format("frame_capture: no frame was drawn within {} ms (the game is at frame {}); a longer "
                                "`timeout`, or a look with `screenshot` at what it shows",
                                wait.count(), render::Frame()));
}

} // namespace

void WatchDraws()
{
    auto &capture = Captured();
    const std::lock_guard hold(capture.lock);
    capture.watcher = render::Watch(&OnDraw, &OnPresent);
}

void AddFrameCapture(Bench &bench)
{
    bench.Tools().Add(
        Read(
            "frame_capture",
            "Records the next frame the game draws, as Direct3D 9 was handed it: each draw call with its vertex "
            "and pixel shaders (by `id`, a hash of the bytecode, the same in every run), their constants by name "
            "with the values the device held (of each register as many floats or ints as the constant's type uses, a "
            "float1 one, a float4x4 sixteen; a bool each), the textures each "
            "sampler had (the .dds file), the primitive counts and the render states asked. What the engine was "
            "drawing: `material` (its .mao, no extension) for a draw a material makes, and for "
            "a mesh's or "
            "cloth's also `model`, `mesh` (the node's name in the model) and `object` and `node`, addresses "
            "that group one object's draws in the frame; none for the interface, full-screen passes and most "
            "effects. `shaders` lists each listed draw's shaders once, with their constant tables. A frame has "
            "some 2,000 draws, so the answer lists `most` (20) of those matching `texture` (part of a .dds "
            "name), `shader` (the start of an id), `material` and `model` (part of the name), after `skip`; "
            "`constants` keeps only the values named. `file` writes every draw of the frame, "
            "unfiltered, for a diff with another engine's. The game must be drawing: a fullscreen game "
            "minimised draws nothing. Recording slows that one frame a little.",
            Json{{"states",
                  {{"type", "array"},
                   {"description", "Render states to read at each draw, by name (ZENABLE, ALPHABLENDENABLE, or "
                                   "D3DRS_ZENABLE) or number. Default: depth, blending, alpha test, cull mode, "
                                   "colour writes, sRGB writes and stencil."}}},
                 {"texture",
                  {{"type", "string"}, {"description", "List only draws sampling a .dds whose name has this."}}},
                 {"shader",
                  {{"type", "string"},
                   {"description", "List only draws whose vertex or pixel shader id begins with this."}}},
                 {"material", {{"type", "string"}, {"description", "List only draws whose material's name has this."}}},
                 {"model", {{"type", "string"}, {"description", "List only draws whose model's name has this."}}},
                 {"constants",
                  {{"type", "array"},
                   {"items", {{"type", "string"}}},
                   {"description", "List only these constants' values, by name. Default: all."}}},
                 {"skip", {{"type", "integer"}, {"description", "Matching draws to pass over first. Default 0."}}},
                 {"most",
                  {{"type", "integer"}, {"description", "Matching draws to list at most, 0 to 100000. Default 20."}}},
                 {"file",
                  {{"type", "string"},
                   {"description", "A path to write the whole frame to as JSON, every draw and its values."}}},
                 {"timeout",
                  {{"type", "integer"},
                   {"description", "Milliseconds to wait for the frame, 100 to 30000. Default 5000."}}}},
            {}, true, Thread::Listener),
        [](const Json &raw) -> ToolResult {
            const Args args(raw, "frame_capture");
            const Json *states = args.Has("states") ? &raw.at("states") : nullptr;
            auto stateList = RenderStatesFrom(states, "frame_capture");
            const auto filter = CaptureFilterFrom(raw, "frame_capture");
            const auto wait = std::chrono::milliseconds(args.Integer("timeout", 5000, 100, 30000));
            const std::string file = args.String("file", "");
            const auto frame = TakeFrame(std::move(stateList), wait);
            Json answer = CaptureJson(frame, filter);
            if (!file.empty())
            {
                CaptureFilter whole;
                whole.most = frame.draws.size();
                std::ofstream out(file, std::ios::binary);
                out << CaptureJson(frame, whole).dump();
                if (!out)
                {
                    throw ToolError(std::format("frame_capture: could not write {}", file));
                }
                answer["file"] = file;
            }
            return answer;
        });
}

} // namespace eluvian::game
