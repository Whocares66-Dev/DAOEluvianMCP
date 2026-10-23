#include "core/FrameCapture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

CapturedShader Shader(std::string id, std::string target)
{
    CapturedShader shader;
    shader.id = std::move(id);
    shader.target = std::move(target);
    shader.bytes = 64;
    return shader;
}

// Three draws: a face with its diffuse map, a wall, and one with no shaders.
FrameCaptured Frame()
{
    FrameCaptured frame;
    frame.frame = 412;

    CapturedDraw face;
    face.index = 0;
    face.call = "DrawIndexedPrimitive";
    face.type = 4;
    face.vertices = 900;
    face.primitives = 1200;
    face.vertex = Shader("aaaa000000000001", "vs_3_0");
    face.vertex.constants.push_back({"g_mWorldViewProj", "float4", 0, 4, std::vector<float>(16, 1.0F), {}, {}});
    face.pixel = Shader("bbbb000000000001", "ps_3_0");
    face.pixel.constants.push_back({"g_vAmbient", "float4", 2, 1, {0.1F, 0.2F, 0.3F, 1.0F}, {}, {}});
    face.pixel.constants.push_back({"g_bLight0", "bool", 0, 1, {}, {}, {true}});
    face.pixel.constants.push_back({"diffuseMap", "sampler", 0, 1, {}, {}, {}});
    face.pixel.textures.push_back({0, "diffuseMap", "uh_hed_elfm_0d.dds"});
    face.states = {{7, 1}, {27, 0}};
    frame.draws.push_back(face);

    CapturedDraw wall = face;
    wall.index = 1;
    wall.pixel = Shader("cccc000000000001", "ps_3_0");
    wall.pixel.textures.push_back({1, "", "bed_wall_stone_0d.dds"});
    frame.draws.push_back(wall);

    CapturedDraw bare;
    bare.index = 2;
    bare.call = "DrawPrimitiveUP";
    bare.type = 5;
    frame.draws.push_back(bare);
    return frame;
}

std::vector<std::byte> Bytes(std::string_view text)
{
    std::vector<std::byte> out;
    for (const char c : text)
    {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

} // namespace

TEST_CASE("frame_capture names a shader by FNV-1a 64 of its bytecode", "[frame_capture]")
{
    // FNV-1a 64's published values: "a" and "foobar".
    CHECK(ShaderId(Bytes("a")) == "af63dc4c8601ec8c");
    CHECK(ShaderId(Bytes("foobar")) == "85944171f73967e8");
    CHECK(ShaderId({}).empty());
}

TEST_CASE("frame_capture reads a render state by name, any case, with or without D3DRS_", "[frame_capture]")
{
    CHECK(RenderStateNumber("ZENABLE") == 7U);
    CHECK(RenderStateNumber("d3drs_alphablendenable") == 27U);
    CHECK(RenderStateNumber("BlendOpAlpha") == 209U);
    CHECK_FALSE(RenderStateNumber("ZENABLED").has_value());
    CHECK(RenderStateName(168) == "COLORWRITEENABLE");
    CHECK(RenderStateName(9999) == "9999");
}

TEST_CASE("frame_capture takes states as names or numbers, the defaults with none", "[frame_capture]")
{
    CHECK(RenderStatesFrom(nullptr, "frame_capture") == DefaultRenderStates());
    const Json given = Json::array({"CULLMODE", 194});
    CHECK(RenderStatesFrom(&given, "frame_capture") == std::vector<std::uint32_t>{22, 194});
    const Json wrong = Json::array({"NOPE"});
    CHECK_THROWS_WITH(RenderStatesFrom(&wrong, "frame_capture"), ContainsSubstring("\"NOPE\""));
    const Json notList = "ZENABLE";
    CHECK_THROWS_AS(RenderStatesFrom(&notList, "frame_capture"), ToolError);
    const Json negative = Json::array({-1});
    CHECK_THROWS_AS(RenderStatesFrom(&negative, "frame_capture"), ToolError);
}

TEST_CASE("frame_capture names primitive types", "[frame_capture]")
{
    CHECK(PrimitiveName(4) == "TRIANGLELIST");
    CHECK(PrimitiveName(6) == "TRIANGLEFAN");
    CHECK(PrimitiveName(0) == "0");
    CHECK(PrimitiveName(7) == "7");
}

TEST_CASE("frame_capture lists a frame's draws, their constants, textures and states", "[frame_capture]")
{
    const Json answer = CaptureJson(Frame(), CaptureFilter{});
    CHECK(answer.at("frame") == 412);
    CHECK(answer.at("draws") == 3);
    CHECK(answer.at("matched") == 3);
    CHECK(answer.at("listed") == 3);
    CHECK(answer.at("truncated") == false);
    CHECK(answer.at("textures") == 2);

    const Json &face = answer.at("list").at(0);
    CHECK(face.at("call") == "DrawIndexedPrimitive");
    CHECK(face.at("primitive") == "TRIANGLELIST");
    CHECK(face.at("vs") == "aaaa000000000001");
    CHECK(face.at("constants").at("ps").at("g_vAmbient") == Json::array({0.1F, 0.2F, 0.3F, 1.0F}));
    CHECK(face.at("constants").at("ps").at("g_bLight0") == Json::array({true}));
    // A sampler's constant is its texture, not a value.
    CHECK_FALSE(face.at("constants").at("ps").contains("diffuseMap"));
    CHECK(face.at("textures").at("ps").at("diffuseMap") == "uh_hed_elfm_0d.dds");
    CHECK(face.at("states") == Json{{"ZENABLE", 1}, {"ALPHABLENDENABLE", 0}});

    // A sampler the table does not name is called by its register.
    CHECK(answer.at("list").at(1).at("textures").at("ps").at("s1") == "bed_wall_stone_0d.dds");

    // No shaders: null ids, no constants or textures.
    const Json &bare = answer.at("list").at(2);
    CHECK(bare.at("vs").is_null());
    CHECK(bare.at("ps").is_null());
    CHECK_FALSE(bare.contains("constants"));
    CHECK_FALSE(bare.contains("textures"));
}

TEST_CASE("frame_capture gives of each register only what the constant uses", "[frame_capture]")
{
    // A float: x alone, the rest another constant's leftovers.
    CapturedConstant scalar{"mml_fOpacityMultiplier", "float4", 5, 1, {1.0F, -7.17F, 1.6F, 0.0F}, {}, {}, 1, 1};
    // A float3x4 kept a column a register: three rows of each of four.
    CapturedConstant columns{"g_mBones", "float4", 8, 4, std::vector<float>(16, 2.0F), {}, {}, 3, 4, true};
    // A float2 array of two: two of each register.
    CapturedConstant pairs{"g_vOffsets", "float4", 12, 2, {1, 2, 3, 4, 5, 6, 7, 8}, {}, {}, 1, 2};
    CHECK(RegisterWidth(scalar) == 1);
    CHECK(RegisterWidth(columns) == 3);
    CHECK(RegisterWidth(pairs) == 2);
    CHECK(RegisterWidth(CapturedConstant{}) == 4);

    FrameCaptured frame = Frame();
    auto &pixel = frame.draws.at(0).pixel.constants;
    pixel.push_back(scalar);
    pixel.push_back(columns);
    pixel.push_back(pairs);
    pixel.push_back({"g_iCount", "int4", 0, 1, {}, {3, 9, 9, 9}, {}, 1, 1});
    const Json values = CaptureJson(frame, CaptureFilter{}).at("list").at(0).at("constants").at("ps");
    CHECK(values.at("mml_fOpacityMultiplier") == Json::array({1.0F}));
    CHECK(values.at("g_mBones").size() == 12);
    CHECK(values.at("g_vOffsets") == Json::array({1.0F, 2.0F, 5.0F, 6.0F}));
    CHECK(values.at("g_iCount") == Json::array({3}));
}

TEST_CASE("frame_capture lists each shader once, with its table", "[frame_capture]")
{
    const Json answer = CaptureJson(Frame(), CaptureFilter{});
    const Json &shaders = answer.at("shaders");
    CHECK(shaders.size() == 3); // one vertex shader, two pixel shaders
    CHECK(shaders.at("aaaa000000000001").at("stage") == "vs");
    CHECK(shaders.at("bbbb000000000001").at("stage") == "ps");
    CHECK(shaders.at("bbbb000000000001").at("constants").size() == 3);
    CHECK(shaders.at("bbbb000000000001").at("constants").at(2).at("set") == "sampler");
}

TEST_CASE("frame_capture keeps the draws sampling a texture, any case", "[frame_capture]")
{
    CaptureFilter filter;
    filter.texture = "ELFM";
    const Json answer = CaptureJson(Frame(), filter);
    CHECK(answer.at("draws") == 3);
    CHECK(answer.at("matched") == 1);
    CHECK(answer.at("list").at(0).at("index") == 0);
    // The frame's textures are counted whatever the filter.
    CHECK(answer.at("textures") == 2);
}

TEST_CASE("frame_capture keeps the draws of a shader by the start of its id", "[frame_capture]")
{
    CaptureFilter filter;
    filter.shader = "cccc";
    const Json answer = CaptureJson(Frame(), filter);
    CHECK(answer.at("matched") == 1);
    CHECK(answer.at("list").at(0).at("index") == 1);
    filter.shader = "aaaa"; // the vertex shader both draws share
    CHECK(CaptureJson(Frame(), filter).at("matched") == 2);
}

TEST_CASE("frame_capture lists only the constants asked for", "[frame_capture]")
{
    CaptureFilter filter;
    filter.constants = {"g_vAmbient"};
    const Json face = CaptureJson(Frame(), filter).at("list").at(0);
    CHECK(face.at("constants").at("ps").size() == 1);
    CHECK(face.at("constants").at("ps").contains("g_vAmbient"));
    CHECK_FALSE(face.at("constants").contains("vs"));
}

TEST_CASE("frame_capture skips and lists at most `most`, and says it is truncated", "[frame_capture]")
{
    CaptureFilter filter;
    filter.skip = 1;
    filter.most = 1;
    const Json answer = CaptureJson(Frame(), filter);
    CHECK(answer.at("matched") == 3);
    CHECK(answer.at("listed") == 1);
    CHECK(answer.at("list").at(0).at("index") == 1);
    CHECK(answer.at("truncated") == true);
    // Only the listed draws' shaders.
    CHECK(answer.at("shaders").size() == 2);

    filter.skip = 2;
    CHECK(CaptureJson(Frame(), filter).at("truncated") == false);
}

TEST_CASE("frame_capture reads its filter from a call", "[frame_capture]")
{
    const auto filter = CaptureFilterFrom(
        Json{{"texture", "dds"}, {"shader", "ab"}, {"constants", {"a", "b"}}, {"skip", 3}, {"most", 7}},
        "frame_capture");
    CHECK(filter.texture == "dds");
    CHECK(filter.shader == "ab");
    CHECK(filter.constants == std::vector<std::string>{"a", "b"});
    CHECK(filter.skip == 3);
    CHECK(filter.most == 7);
    CHECK(CaptureFilterFrom(Json::object(), "frame_capture").most == 20);
    CHECK_THROWS_WITH(CaptureFilterFrom(Json{{"constants", {1}}}, "frame_capture"), ContainsSubstring("`constants`"));
    CHECK_THROWS_AS(CaptureFilterFrom(Json{{"most", -1}}, "frame_capture"), ToolError);
}
