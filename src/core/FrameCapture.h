// `frame_capture`: one frame's draws as Direct3D 9 was handed them, each with
// its shaders, their constants by name, the textures they sample and the
// render states asked for (#160). For DAOParagonEngine's renderer checked
// draw by draw against the game's, and for a shader plugin's work, which
// took a capture plugin of its own (elgara-agent's char-capture, 2026-10-19).
// src/game takes the draws through DAOLib's dao::render watch (DAOLib #246)
// and copies them into the plain records here; what is decided over them,
// the shaders' ids, the render states' names, which draws a call asked for
// and the answer's shape, is here, tested.
#pragma once

#include "core/Tool.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eluvian
{

// A shader's id: FNV-1a 64 of its bytecode, 16 hex digits. The same shader
// has the same id in every run and under any graphics plugin that leaves it
// alone; "" for no bytecode (the fixed pipeline).
std::string ShaderId(std::span<const std::byte> bytecode);

// A render state's number (D3DRENDERSTATETYPE) by its name, with or without
// "D3DRS_", in any case; nothing for a name d3d9types.h does not have.
std::optional<std::uint32_t> RenderStateNumber(std::string_view name);
// A render state's name without "D3DRS_"; its number as text for one with
// none.
std::string RenderStateName(std::uint32_t state);

// The states a capture reads at each draw when a call names none: depth,
// blending, alpha test, culling and colour writes, what tells one pass from
// another.
const std::vector<std::uint32_t> &DefaultRenderStates();

// `states` as a call gives it: names or numbers. Absent, the defaults.
// Throws ToolError naming the tool for one that is neither.
std::vector<std::uint32_t> RenderStatesFrom(const Json *given, std::string_view tool);

// One constant of a shader's table with what the device held in its
// registers: four floats or four ints a register, one bool a register.
struct CapturedConstant
{
    std::string name;
    std::string set; // "float4", "int4", "bool" or "sampler"
    std::uint16_t index = 0;
    std::uint16_t count = 0;
    std::vector<float> floats;
    std::vector<int> ints;
    std::vector<bool> bools;
    // Its type's shape in the table: what of each register it uses.
    std::uint16_t rows = 0;
    std::uint16_t columns = 0;
    bool columnMajor = false; // a matrix kept a column a register
};

// How many of a register's four values a constant uses: its columns, or
// its rows for a column-major matrix; all four where the table says
// nothing. The rest are what the register held before, another constant's
// (paragon-agent's helper read mml_fOpacityMultiplier as
// [1.0, -7.17, 1.6, 0.0], 2026-10-23).
std::size_t RegisterWidth(const CapturedConstant &constant);

// A texture a shader samples: its sampler's register and name in the
// shader's table, and the .dds it was made from ("" if none).
struct CapturedTexture
{
    std::uint32_t sampler = 0;
    std::string samplerName;
    std::string file;
};

// A shader bound for a draw; `id` "" for none.
struct CapturedShader
{
    std::string id;
    std::string target; // "ps_3_0"; "" without a table
    std::size_t bytes = 0;
    std::vector<CapturedConstant> constants;
    std::vector<CapturedTexture> textures;
};

// One draw call.
struct CapturedDraw
{
    std::uint32_t index = 0;
    std::string call;       // "DrawIndexedPrimitive", ...
    std::uint32_t type = 0; // D3DPRIMITIVETYPE
    std::int32_t baseVertex = 0;
    std::uint32_t minIndex = 0;
    std::uint32_t vertices = 0;
    std::uint32_t start = 0;
    std::uint32_t primitives = 0;
    CapturedShader vertex;
    CapturedShader pixel;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> states; // state, value
};

struct FrameCaptured
{
    std::uint64_t frame = 0;
    std::vector<CapturedDraw> draws;
};

// Which draws a call asked for and how much of each.
struct CaptureFilter
{
    std::string texture;                // draws sampling a .dds whose name has this, any case
    std::string shader;                 // draws whose vertex or pixel shader's id begins with this
    std::vector<std::string> constants; // only these constants' values; empty, all
    std::size_t skip = 0;               // matching draws passed over first
    std::size_t most = 20;              // matching draws listed at most
};

// The filter as a call gives it (texture, shader, constants, skip, most);
// throws ToolError naming the tool for a wrong one.
CaptureFilter CaptureFilterFrom(const Json &raw, std::string_view tool);

// A primitive type's name (D3DPRIMITIVETYPE), "TRIANGLELIST"; its number as
// text for one there is not.
std::string PrimitiveName(std::uint32_t type);

// The answer: `frame`, `draws` (all of the frame's), `matched`, `listed`,
// `truncated`, `textures` (how many .dds the frame's draws sample, each
// once), `shaders` (each listed draw's shaders once, by id: stage, target,
// size and its table's constants) and `list`, the draws themselves.
Json CaptureJson(const FrameCaptured &captured, const CaptureFilter &filter);

} // namespace eluvian
