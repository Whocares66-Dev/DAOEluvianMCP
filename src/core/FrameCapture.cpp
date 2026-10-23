// What is decided over a frame's draws. See FrameCapture.h.
#include "core/FrameCapture.h"

#include "core/Args.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <limits>
#include <set>

namespace eluvian
{

namespace
{

// D3DRENDERSTATETYPE, as d3d9types.h has it (Windows Kits 10.0.22621.0,
// read 2026-10-23), without "D3DRS_".
struct NamedState
{
    std::string_view name;
    std::uint32_t number;
};
constexpr std::array kStates = std::to_array<NamedState>({
    {"ZENABLE", 7},
    {"FILLMODE", 8},
    {"SHADEMODE", 9},
    {"ZWRITEENABLE", 14},
    {"ALPHATESTENABLE", 15},
    {"LASTPIXEL", 16},
    {"SRCBLEND", 19},
    {"DESTBLEND", 20},
    {"CULLMODE", 22},
    {"ZFUNC", 23},
    {"ALPHAREF", 24},
    {"ALPHAFUNC", 25},
    {"DITHERENABLE", 26},
    {"ALPHABLENDENABLE", 27},
    {"FOGENABLE", 28},
    {"SPECULARENABLE", 29},
    {"FOGCOLOR", 34},
    {"FOGTABLEMODE", 35},
    {"FOGSTART", 36},
    {"FOGEND", 37},
    {"FOGDENSITY", 38},
    {"RANGEFOGENABLE", 48},
    {"STENCILENABLE", 52},
    {"STENCILFAIL", 53},
    {"STENCILZFAIL", 54},
    {"STENCILPASS", 55},
    {"STENCILFUNC", 56},
    {"STENCILREF", 57},
    {"STENCILMASK", 58},
    {"STENCILWRITEMASK", 59},
    {"TEXTUREFACTOR", 60},
    {"WRAP0", 128},
    {"WRAP1", 129},
    {"WRAP2", 130},
    {"WRAP3", 131},
    {"WRAP4", 132},
    {"WRAP5", 133},
    {"WRAP6", 134},
    {"WRAP7", 135},
    {"CLIPPING", 136},
    {"LIGHTING", 137},
    {"AMBIENT", 139},
    {"FOGVERTEXMODE", 140},
    {"COLORVERTEX", 141},
    {"LOCALVIEWER", 142},
    {"NORMALIZENORMALS", 143},
    {"DIFFUSEMATERIALSOURCE", 145},
    {"SPECULARMATERIALSOURCE", 146},
    {"AMBIENTMATERIALSOURCE", 147},
    {"EMISSIVEMATERIALSOURCE", 148},
    {"VERTEXBLEND", 151},
    {"CLIPPLANEENABLE", 152},
    {"POINTSIZE", 154},
    {"POINTSIZE_MIN", 155},
    {"POINTSPRITEENABLE", 156},
    {"POINTSCALEENABLE", 157},
    {"POINTSCALE_A", 158},
    {"POINTSCALE_B", 159},
    {"POINTSCALE_C", 160},
    {"MULTISAMPLEANTIALIAS", 161},
    {"MULTISAMPLEMASK", 162},
    {"PATCHEDGESTYLE", 163},
    {"DEBUGMONITORTOKEN", 165},
    {"POINTSIZE_MAX", 166},
    {"INDEXEDVERTEXBLENDENABLE", 167},
    {"COLORWRITEENABLE", 168},
    {"TWEENFACTOR", 170},
    {"BLENDOP", 171},
    {"POSITIONDEGREE", 172},
    {"NORMALDEGREE", 173},
    {"SCISSORTESTENABLE", 174},
    {"SLOPESCALEDEPTHBIAS", 175},
    {"ANTIALIASEDLINEENABLE", 176},
    {"MINTESSELLATIONLEVEL", 178},
    {"MAXTESSELLATIONLEVEL", 179},
    {"ADAPTIVETESS_X", 180},
    {"ADAPTIVETESS_Y", 181},
    {"ADAPTIVETESS_Z", 182},
    {"ADAPTIVETESS_W", 183},
    {"ENABLEADAPTIVETESSELLATION", 184},
    {"TWOSIDEDSTENCILMODE", 185},
    {"CCW_STENCILFAIL", 186},
    {"CCW_STENCILZFAIL", 187},
    {"CCW_STENCILPASS", 188},
    {"CCW_STENCILFUNC", 189},
    {"COLORWRITEENABLE1", 190},
    {"COLORWRITEENABLE2", 191},
    {"COLORWRITEENABLE3", 192},
    {"BLENDFACTOR", 193},
    {"SRGBWRITEENABLE", 194},
    {"DEPTHBIAS", 195},
    {"WRAP8", 198},
    {"WRAP9", 199},
    {"WRAP10", 200},
    {"WRAP11", 201},
    {"WRAP12", 202},
    {"WRAP13", 203},
    {"WRAP14", 204},
    {"WRAP15", 205},
    {"SEPARATEALPHABLENDENABLE", 206},
    {"SRCBLENDALPHA", 207},
    {"DESTBLENDALPHA", 208},
    {"BLENDOPALPHA", 209},
});

std::string Upper(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

bool Contains(std::string_view text, std::string_view part)
{
    return Upper(text).find(Upper(part)) != std::string::npos;
}

bool Samples(const CapturedDraw &draw, std::string_view texture)
{
    const auto has = [texture](const CapturedShader &shader) {
        return std::ranges::any_of(shader.textures, [texture](const CapturedTexture &sampled) {
            return !sampled.file.empty() && Contains(sampled.file, texture);
        });
    };
    return has(draw.vertex) || has(draw.pixel);
}

bool Runs(const CapturedDraw &draw, std::string_view shader)
{
    const auto starts = [shader](const CapturedShader &bound) {
        return !bound.id.empty() && bound.id.starts_with(shader);
    };
    return starts(draw.vertex) || starts(draw.pixel);
}

bool Matches(const CapturedDraw &draw, const CaptureFilter &filter)
{
    return (filter.texture.empty() || Samples(draw, filter.texture)) &&
           (filter.shader.empty() || Runs(draw, filter.shader)) &&
           (filter.material.empty() || (!draw.material.empty() && Contains(draw.material, filter.material))) &&
           (filter.model.empty() || (!draw.model.empty() && Contains(draw.model, filter.model)));
}

// Of four values a register, the first `width` of each.
template <class T> Json Trimmed(const std::vector<T> &values, std::size_t width)
{
    Json out = Json::array();
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        if (i % 4 < width)
        {
            out.push_back(values[i]);
        }
    }
    return out;
}

// A constant's registers as they were: the floats, ints or bools its set
// has, each register's trimmed to what the constant uses.
Json ValueJson(const CapturedConstant &constant)
{
    if (!constant.ints.empty())
    {
        return Trimmed(constant.ints, RegisterWidth(constant));
    }
    if (!constant.bools.empty())
    {
        return constant.bools;
    }
    return Trimmed(constant.floats, RegisterWidth(constant));
}

// A shader's constants by name, those the filter asks for; null for none.
Json ConstantsJson(const CapturedShader &shader, const std::vector<std::string> &wanted)
{
    Json out = Json::object();
    for (const auto &constant : shader.constants)
    {
        if (constant.set == "sampler")
        {
            continue; // its texture is under `textures`
        }
        if (!wanted.empty() && std::ranges::find(wanted, constant.name) == wanted.end())
        {
            continue;
        }
        out[constant.name] = ValueJson(constant);
    }
    return out;
}

// A shader's textures by its sampler's name, or `s<register>` without one.
Json TexturesJson(const CapturedShader &shader)
{
    Json out = Json::object();
    for (const auto &sampled : shader.textures)
    {
        const std::string name =
            sampled.samplerName.empty() ? std::format("s{}", sampled.sampler) : sampled.samplerName;
        out[name] = sampled.file.empty() ? Json(nullptr) : Json(sampled.file);
    }
    return out;
}

// A stage's part of a draw, under "vs" and "ps", left out where it is empty.
void PutStages(Json &into, const char *key, const Json &vertex, const Json &pixel)
{
    Json stages = Json::object();
    if (!vertex.empty())
    {
        stages["vs"] = vertex;
    }
    if (!pixel.empty())
    {
        stages["ps"] = pixel;
    }
    if (!stages.empty())
    {
        into[key] = std::move(stages);
    }
}

Json DrawJson(const CapturedDraw &draw, const CaptureFilter &filter)
{
    Json out = {{"index", draw.index},
                {"call", draw.call},
                {"primitive", PrimitiveName(draw.type)},
                {"primitives", draw.primitives},
                {"vertices", draw.vertices},
                {"start", draw.start},
                {"baseVertex", draw.baseVertex},
                {"minIndex", draw.minIndex},
                {"vs", draw.vertex.id.empty() ? Json(nullptr) : Json(draw.vertex.id)},
                {"ps", draw.pixel.id.empty() ? Json(nullptr) : Json(draw.pixel.id)}};
    if (!draw.material.empty())
    {
        out["material"] = draw.material;
    }
    if (draw.object != 0)
    {
        out["object"] = std::format("0x{:x}", draw.object);
        out["model"] = draw.model;
    }
    if (draw.node != 0)
    {
        out["node"] = std::format("0x{:x}", draw.node);
        out["mesh"] = draw.mesh;
    }
    PutStages(out, "constants", ConstantsJson(draw.vertex, filter.constants),
              ConstantsJson(draw.pixel, filter.constants));
    PutStages(out, "textures", TexturesJson(draw.vertex), TexturesJson(draw.pixel));
    Json states = Json::object();
    for (const auto &[state, value] : draw.states)
    {
        states[RenderStateName(state)] = value;
    }
    out["states"] = std::move(states);
    return out;
}

// A shader once: its stage, target, size and its table, without the values.
Json ShaderJson(const CapturedShader &shader, const char *stage)
{
    Json constants = Json::array();
    for (const auto &constant : shader.constants)
    {
        constants.push_back(
            {{"name", constant.name}, {"set", constant.set}, {"index", constant.index}, {"count", constant.count}});
    }
    return {{"stage", stage}, {"target", shader.target}, {"bytes", shader.bytes}, {"constants", std::move(constants)}};
}

} // namespace

std::string ShaderId(std::span<const std::byte> bytecode)
{
    if (bytecode.empty())
    {
        return {};
    }
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const std::byte b : bytecode)
    {
        hash ^= static_cast<std::uint64_t>(b);
        hash *= 0x100000001b3ULL;
    }
    return std::format("{:016x}", hash);
}

std::size_t RegisterWidth(const CapturedConstant &constant)
{
    const std::size_t width = constant.columnMajor ? constant.rows : constant.columns;
    return width >= 1 && width <= 4 ? width : 4;
}

std::optional<std::uint32_t> RenderStateNumber(std::string_view name)
{
    std::string wanted = Upper(name);
    if (wanted.starts_with("D3DRS_"))
    {
        wanted.erase(0, 6);
    }
    for (const auto &state : kStates)
    {
        if (state.name == wanted)
        {
            return state.number;
        }
    }
    return std::nullopt;
}

std::string RenderStateName(std::uint32_t state)
{
    for (const auto &named : kStates)
    {
        if (named.number == state)
        {
            return std::string(named.name);
        }
    }
    return std::to_string(state);
}

const std::vector<std::uint32_t> &DefaultRenderStates()
{
    // ZENABLE, ZWRITEENABLE, ZFUNC, ALPHATESTENABLE, ALPHAREF,
    // ALPHABLENDENABLE, SRCBLEND, DESTBLEND, BLENDOP, CULLMODE,
    // COLORWRITEENABLE, SRGBWRITEENABLE, STENCILENABLE.
    static const std::vector<std::uint32_t> states = {7, 14, 23, 15, 24, 27, 19, 20, 171, 22, 168, 194, 52};
    return states;
}

std::vector<std::uint32_t> RenderStatesFrom(const Json *given, std::string_view tool)
{
    if (given == nullptr || given->is_null())
    {
        return DefaultRenderStates();
    }
    if (!given->is_array())
    {
        throw ToolError(std::format("{}: `states` is a list of render states, by name (ZENABLE) or number", tool));
    }
    std::vector<std::uint32_t> states;
    for (const auto &state : *given)
    {
        if (state.is_number_integer() && state.get<std::int64_t>() >= 0 &&
            state.get<std::int64_t>() <= std::numeric_limits<std::uint32_t>::max())
        {
            states.push_back(static_cast<std::uint32_t>(state.get<std::int64_t>()));
        }
        else if (state.is_string())
        {
            const auto number = RenderStateNumber(state.get<std::string>());
            if (!number)
            {
                throw ToolError(std::format("{}: no render state is called \"{}\"; names are d3d9types.h's "
                                            "D3DRS_*, with or without the prefix (ALPHABLENDENABLE)",
                                            tool, state.get<std::string>()));
            }
            states.push_back(*number);
        }
        else
        {
            throw ToolError(
                std::format("{}: `states` has {}, which is neither a name nor a number", tool, state.dump()));
        }
    }
    return states;
}

CaptureFilter CaptureFilterFrom(const Json &raw, std::string_view tool)
{
    const Args args(raw, std::string(tool));
    CaptureFilter filter;
    filter.texture = args.String("texture", "");
    filter.shader = args.String("shader", "");
    filter.material = args.String("material", "");
    filter.model = args.String("model", "");
    if (args.Has("constants"))
    {
        const Json &constants = raw.at("constants");
        if (!constants.is_array() || !std::ranges::all_of(constants, [](const Json &name) { return name.is_string(); }))
        {
            throw ToolError(std::format("{}: `constants` is a list of constants' names (g_mWorldViewProj)", tool));
        }
        filter.constants = constants.get<std::vector<std::string>>();
    }
    filter.skip = static_cast<std::size_t>(args.Integer("skip", 0, 0, 1'000'000));
    filter.most = static_cast<std::size_t>(args.Integer("most", 20, 0, 100'000));
    return filter;
}

std::string PrimitiveName(std::uint32_t type)
{
    static constexpr std::array<std::string_view, 7> kNames = {
        "", "POINTLIST", "LINELIST", "LINESTRIP", "TRIANGLELIST", "TRIANGLESTRIP", "TRIANGLEFAN"};
    if (type >= 1 && type < kNames.size())
    {
        return std::string(kNames.at(type));
    }
    return std::to_string(type);
}

Json CaptureJson(const FrameCaptured &captured, const CaptureFilter &filter)
{
    std::set<std::string, std::less<>> textures;
    std::set<std::string, std::less<>> materials;
    for (const auto &draw : captured.draws)
    {
        if (!draw.material.empty())
        {
            materials.insert(draw.material);
        }
        for (const CapturedShader *shader : {&draw.vertex, &draw.pixel})
        {
            for (const auto &sampled : shader->textures)
            {
                if (!sampled.file.empty())
                {
                    textures.insert(sampled.file);
                }
            }
        }
    }
    Json list = Json::array();
    Json shaders = Json::object();
    std::size_t matched = 0;
    for (const auto &draw : captured.draws)
    {
        if (!Matches(draw, filter))
        {
            continue;
        }
        ++matched;
        if (matched <= filter.skip || list.size() >= filter.most)
        {
            continue;
        }
        list.push_back(DrawJson(draw, filter));
        if (!draw.vertex.id.empty() && !shaders.contains(draw.vertex.id))
        {
            shaders[draw.vertex.id] = ShaderJson(draw.vertex, "vs");
        }
        if (!draw.pixel.id.empty() && !shaders.contains(draw.pixel.id))
        {
            shaders[draw.pixel.id] = ShaderJson(draw.pixel, "ps");
        }
    }
    const std::size_t listed = list.size();
    return {{"frame", captured.frame},
            {"draws", captured.draws.size()},
            {"matched", matched},
            {"listed", listed},
            {"truncated", matched > filter.skip + listed},
            {"textures", textures.size()},
            {"materials", materials.size()},
            {"shaders", std::move(shaders)},
            {"list", std::move(list)}};
}

} // namespace eluvian
