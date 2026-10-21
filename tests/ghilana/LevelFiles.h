// A level, a stage and a cutscene made in a test, laid out as DAOLib's own
// tests of their readers lay them out (tests/test_level.cpp, test_stage.cpp,
// test_cutscene.cpp, over its tests/gff4_builder.h), for ghilana's level,
// atmosphere, stage and cutscene (#163). Those builders are in their tests'
// own files, so they are copied here with the level's room placing its
// layout as the game's rooms do.
#pragma once

#include "gff4_builder.h"

#include <dao/core/Gff4.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ghtest::levels
{

namespace gff4 = dao::core::gff4;
using dao::test::Builder;
using dao::test::Children;
using dao::test::Float;
using dao::test::Floats;
using dao::test::kGeneric;
using dao::test::Start;
using dao::test::Struct;

constexpr std::uint16_t kStructList = gff4::kList | gff4::kStruct;

// lak303d.arl: level 7, "Circle Tower", room 3672 "lak303d_3672" from (-1, -2, -3) to (4, 5, 6), a
// sun along -z of colour (0.5, 0.25, 0.125, 1), da_hsvmatrix with a Brightness of 0.75, an
// atmosphere of a sun's intensity of 19.95 and a field DAOLib has no name for (22999) of 3.
inline std::vector<std::byte> LevelLayoutFile()
{
    Builder b = Start("ARL ", {{"AREA",
                                80,
                                {{3011, 5, 0, 0},
                                 {3012, 14, 0, 4},
                                 {3016, 1, kStructList, 8},
                                 {3024, 10, 0, 12},
                                 {3026, 2, kStructList, 24},
                                 {3150, 10, 0, 28},
                                 {3152, 15, 0, 40},
                                 {3122, 1, 0, 56},
                                 {3123, 5, 0, 60},
                                 {3134, 1, 0, 64},
                                 {22500, 4, gff4::kStruct, 68},
                                 {3355, 5, kStructList, 76}}},
                               {"ROOM", 32, {{3031, 5, 0, 0}, {3033, 14, 0, 4}, {3140, 10, 0, 8}, {3141, 10, 0, 20}}},
                               {"FBE", 12, {{5520, 14, 0, 0}, {5521, 14, 0, 4}, {5522, 3, kStructList, 8}}},
                               {"FBEP", 12, {{5523, 14, 0, 0}, {5524, kGeneric, gff4::kReference, 4}}},
                               {"ATMO", 8, {{22520, 8, 0, 0}, {22999, 8, 0, 4}}},
                               {"SPT", 4, {{3353, 14, 0, 0}}}});
    const std::size_t root = Struct(b, 80);
    b.U32(root, 7);
    b.U32(root + 4, b.String(u"Circle Tower"));
    Floats(b, root + 12, {1, 2, 3});
    Floats(b, root + 28, {0, 0, -1});
    Floats(b, root + 40, {0.5F, 0.25F, 0.125F, 1});
    b.U8(root + 56, 1);
    b.U32(root + 60, 10);
    b.U8(root + 64, 1);
    Float(b, root + 68, 19.95F);
    Float(b, root + 72, 3.0F);

    const std::size_t rooms = b.bytes.size();
    b.U32(rooms, 1);
    const std::size_t room = Struct(b, 32);
    b.U32(root + 8, static_cast<std::uint32_t>(rooms - b.data));
    b.U32(room, 3672);
    b.U32(room + 4, b.String(u"lak303d_3672"));
    Floats(b, room + 8, {-1, -2, -3, 4, 5, 6});

    const std::size_t effects = b.bytes.size();
    b.U32(effects, 1);
    const std::size_t effect = Struct(b, 12);
    b.U32(root + 24, static_cast<std::uint32_t>(effects - b.data));
    b.U32(effect, b.String(u"da_hsvmatrix"));
    b.U32(effect + 4, b.String(u"da_hsvmatrix"));
    const std::size_t parameters = b.bytes.size();
    b.U32(parameters, 1);
    const std::size_t parameter = Struct(b, 12);
    b.U32(effect + 8, static_cast<std::uint32_t>(parameters - b.data));
    b.U32(parameter, b.String(u"Brightness"));
    b.U32(parameter + 4, static_cast<std::uint32_t>(gff4::Type::F32));
    const std::size_t value = b.bytes.size();
    Float(b, value, 0.75F);
    b.U32(parameter + 8, static_cast<std::uint32_t>(value - b.data));

    const std::size_t trees = b.bytes.size();
    b.U32(trees, 1);
    const std::size_t tree = Struct(b, 4);
    b.U32(root + 76, static_cast<std::uint32_t>(trees - b.data));
    b.U32(tree, b.String(u"Lak100d_tre_m_mossy"));
    return b.bytes;
}

// lak303d_3672.rml: the room at (100, 0, 0), turned a quarter about z, seeing rooms 7 and 9 and
// opening onto 9 and 12. It places the model "slab" at (0, 0, 5), twice the size, its part "_1" in the atlas
// "room.dds" at an offset of (0.5, 0.25) and a scale of (0.25, 0.5); then its layout
// "lak303d_3672_ll" at its origin, as the game's rooms place theirs; then "fxe_torch" at (1, 1, 0),
// turned a quarter about z too.
inline std::vector<std::byte> RoomFile()
{
    Builder b = Start(
        "RML ", {{"ROOM",
                  44,
                  {{4, 10, 0, 0},
                   {5, 13, 0, 16},
                   {3047, 5, gff4::kList, 32},
                   {3050, 2, kStructList, 36},
                   {3099, 5, gff4::kList, 40}}},
                 {"LMAT", 24, {{3324, 12, 0, 0}, {3323, 14, 0, 16}, {3235, 14, 0, 20}}},
                 {"MDL ",
                  48,
                  {{4, 10, 0, 0}, {5, 13, 0, 16}, {3059, 8, 0, 32}, {3063, 14, 0, 36}, {3326, 1, kStructList, 40}}}});
    const std::size_t root = Struct(b, 44);
    Floats(b, root, {100, 0, 0});
    Floats(b, root + 16, {0, 0, 0.70710678F, 0.70710678F});
    b.U32(root + 32, b.Numbers({7, 9}));
    b.U32(root + 40, b.Numbers({9, 12}));
    const std::size_t models = b.bytes.size();
    b.U32(models, 3);
    const std::size_t slab = Struct(b, 48);
    const std::size_t layout = Struct(b, 48);
    const std::size_t torch = Struct(b, 48);
    b.U32(root + 36, static_cast<std::uint32_t>(models - b.data));
    Floats(b, slab, {0, 0, 5});
    Floats(b, slab + 16, {0, 0, 0, 1});
    Float(b, slab + 32, 2.0F);
    b.U32(slab + 36, b.String(u"slab"));
    const std::size_t parts = b.bytes.size();
    b.U32(parts, 1);
    const std::size_t part = Struct(b, 24);
    b.U32(slab + 40, static_cast<std::uint32_t>(parts - b.data));
    Floats(b, part, {0.5F, 0.25F, 0.25F, 0.5F});
    b.U32(part + 16, b.String(u"room.dds"));
    b.U32(part + 20, b.String(u"_1"));
    Floats(b, layout, {0, 0, 0});
    Floats(b, layout + 16, {0, 0, 0, 1});
    Float(b, layout + 32, 1.0F);
    b.U32(layout + 36, b.String(u"lak303d_3672_ll"));
    b.U32(layout + 40, b.Numbers({}));
    Floats(b, torch, {1, 1, 0});
    Floats(b, torch + 16, {0, 0, 0.70710678F, 0.70710678F});
    Float(b, torch + 32, 1.0F);
    b.U32(torch + 36, b.String(u"fxe_torch"));
    b.U32(torch + 40, b.Numbers({}));
    return b.bytes;
}

// lak303d_3672_ll.mmh, the room's layout: "GOB" at (1, 0, 0) turned a quarter about z, and under
// it a flickering lamp "Lamp_RT01" at (2, 0, 0) of radius 8, the ambient light "Ambient0", and a
// probe at (0, 0, 3) with its cube and its irradiance, baked.
inline std::vector<std::byte> LayoutFile()
{
    const std::uint16_t children = gff4::kList | gff4::kReference;
    Builder b = Start(
        "MMH ", {{"mdlh", 4, {{6999, kGeneric, children, 0}}},
                 {"node", 8, {{6000, 14, 0, 0}, {6999, kGeneric, children, 4}}},
                 {"trsl", 16, {{6047, 12, 0, 0}}},
                 {"rota", 16, {{6048, 13, 0, 0}}},
                 {"nplt",
                  64,
                  {{6000, 14, 0, 16},
                   {6004, 5, 0, 20},
                   {6007, 15, 0, 0},
                   {6008, 8, 0, 24},
                   {6009, 0, 0, 44},
                   {6249, 8, 0, 28},
                   {6250, 8, 0, 32},
                   {6251, 8, 0, 36},
                   {6296, 0, 0, 45},
                   {6339, 0, 0, 46},
                   {6340, 0, 0, 48},
                   {6345, 0, 0, 47},
                   {6999, kGeneric, children, 40}}},
                 {"nalt", 32, {{6000, 14, 0, 16}, {6010, 15, 0, 0}, {6340, 0, 0, 24}, {6999, kGeneric, children, 20}}},
                 {"nlpr", 8, {{6000, 14, 0, 0}, {6999, kGeneric, children, 4}}},
                 {"nrfl", 4, {{3104, 14, 0, 0}}},
                 {"nirr", 208, {{6253, 14, 0, 192}, {6336, 16, 0, 0}, {6337, 16, 0, 64}, {6338, 16, 0, 128}}}});
    const std::size_t root = Struct(b, 4);
    const std::size_t gob = Struct(b, 8);
    const std::size_t gobAt = Struct(b, 16);
    const std::size_t gobTurn = Struct(b, 16);
    const std::size_t lamp = Struct(b, 64);
    const std::size_t lampAt = Struct(b, 16);
    const std::size_t ambient = Struct(b, 32);
    const std::size_t probe = Struct(b, 8);
    const std::size_t probeAt = Struct(b, 16);
    const std::size_t cube = Struct(b, 4);
    const std::size_t irradiance = Struct(b, 208);
    Floats(b, gobAt, {1, 0, 0, 1});
    Floats(b, gobTurn, {0, 0, 0.70710678F, 0.70710678F});
    Floats(b, lampAt, {2, 0, 0, 1});
    Floats(b, probeAt, {0, 0, 3, 1});
    Floats(b, lamp, {1.82F, 0.96F, 0.30F, 1});
    Floats(b, lamp + 24, {8, 0.25F, 0.075F, 0.025F});
    b.U8(lamp + 44, 1);
    b.U8(lamp + 45, 2);
    b.U8(lamp + 47, 1);
    Floats(b, ambient, {0.104F, 0.112F, 0.108F, 1});
    for (std::size_t i = 0; i < 16; ++i)
    {
        Float(b, irradiance + (4 * i), static_cast<float>(i));
    }
    b.U32(root, Children(b, {{1, gob}}));
    b.U32(gob, b.String(u"GOB"));
    b.U32(gob + 4, Children(b, {{2, gobAt}, {3, gobTurn}, {4, lamp}, {5, ambient}, {6, probe}}));
    b.U32(lamp + 16, b.String(u"Lamp_RT01"));
    b.U32(lamp + 40, Children(b, {{2, lampAt}}));
    b.U32(ambient + 16, b.String(u"Ambient0"));
    b.U32(probe, b.String(u"NewLightProbe"));
    b.U32(probe + 4, Children(b, {{2, probeAt}, {7, cube}, {8, irradiance}}));
    b.U32(cube, b.String(u"lak303d_probe_2zz.dds"));
    b.U32(irradiance + 192, b.String(u"lak303d_probe_2zz.mtx"));
    return b.bytes;
}

// jowan_attack.stg: the places "duncan" at (-3, 0.25, 0.5), its default camera "group", and
// "irving"; the camera "group" at (0.5, -7.5, 1.5) of 45 degrees, with both places in shot,
// looking from duncan at irving and greagoir, LookingAtType 3.
inline std::vector<std::byte> StageFile()
{
    Builder b = Start("STG ", {{"STAG", 8, {{11000, 1, kStructList, 0}, {11001, 2, kStructList, 4}}},
                               {"PLAC", 0x30, {{1, 14, 0, 32}, {4, 10, 0, 0}, {5, 13, 0, 16}, {11004, 14, 0, 36}}},
                               {"CAMR",
                                0x40,
                                {{1, 14, 0, 32},
                                 {4, 10, 0, 0},
                                 {5, 13, 0, 16},
                                 {11002, 14, gff4::kList, 40},
                                 {11003, 8, 0, 36},
                                 {11006, 14, 0, 44},
                                 {11007, 14, 0, 48},
                                 {11008, 14, 0, 52},
                                 {11009, 0, 0, 56}}}});
    const std::size_t root = Struct(b, 8);
    const std::size_t places = b.bytes.size();
    b.U32(places, 2);
    const std::size_t first = Struct(b, 0x30);
    const std::size_t second = Struct(b, 0x30);
    b.U32(root, static_cast<std::uint32_t>(places - b.data));
    Floats(b, first, {-3.0F, 0.25F, 0.5F});
    Floats(b, first + 16, {0.0F, 0.0F, -0.5F, 0.75F});
    b.U32(first + 32, b.String(u"duncan"));
    b.U32(first + 36, b.String(u"group"));
    Floats(b, second, {1.0F, 2.0F, 3.0F});
    Floats(b, second + 16, {0.0F, 0.0F, 0.0F, 1.0F});
    b.U32(second + 32, b.String(u"irving"));
    b.U32(second + 36, 0xFFFFFFFF);

    const std::size_t cameras = b.bytes.size();
    b.U32(cameras, 1);
    const std::size_t camera = Struct(b, 0x40);
    b.U32(root + 4, static_cast<std::uint32_t>(cameras - b.data));
    Floats(b, camera, {0.5F, -7.5F, 1.5F});
    Floats(b, camera + 16, {-0.25F, -0.125F, 0.0625F, 0.5F});
    b.U32(camera + 32, b.String(u"group"));
    Float(b, camera + 36, 45.0F);
    const std::uint32_t duncan = b.String(u"duncan");
    const std::uint32_t irving = b.String(u"irving");
    b.U32(camera + 40, b.Numbers({duncan, irving}));
    b.U32(camera + 44, b.String(u"duncan"));
    b.U32(camera + 48, b.String(u"irving"));
    b.U32(camera + 52, b.String(u"greagoir"));
    b.U8(camera + 56, 3);
    return b.bytes;
}

// A list of floats at the data's end: its data offset.
inline std::uint32_t FloatList(Builder &b, const std::vector<float> &values)
{
    const std::size_t at = b.bytes.size();
    b.U32(at, static_cast<std::uint32_t>(values.size()));
    Floats(b, at + 4, values);
    return static_cast<std::uint32_t>(at - b.data);
}

// bhmcs_intro.cut, cut down: 61 s at 30 frames a second in bhm500ar_tower_harrowing, its end
// script gen00cs_cutscene_end; one actor, "Jowan" (7), whose queue is an animation mag_cast from
// 2 s to the end with one curve (base -6.5, one vertex (41.5, -20.25)), the spoken line 6081234
// from 3 to 5.5 s, and a stage camera by the henchman's place.
inline std::vector<std::byte> CutsceneFile()
{
    Builder b = Start(
        "CUT ",
        {{"CUT",
          40,
          {{3010, 14, 0, 0},
           {5000, 8, 0, 4},
           {5001, 14, 0, 8},
           {5007, 14, 0, 12},
           {5016, 4, 0, 16},
           {5017, 14, 0, 20},
           {5018, 0, 0, 24},
           {5019, 0, 0, 25},
           {5050, 14, 0, 28},
           {5100, 14, gff4::kList, 32},
           {5200, 6, kStructList, 36}}},
         {"VERT", 8, {{5370, 8, 0, 0}, {5371, 8, 0, 4}}},
         {"TRAN", 20, {{5380, 0, 0, 0}, {5381, 1, gff4::kStruct, 4}, {5382, 1, gff4::kStruct, 12}}},
         {"CURV", 12, {{5350, 8, 0, 0}, {5351, 1, kStructList, 4}, {5352, 2, kStructList, 8}}},
         {"AANI",
          48,
          {{5300, 0, 0, 0},
           {5301, 8, 0, 4},
           {5302, 8, 0, 8},
           {5303, 3, kStructList, 12},
           {5304, 0, 0, 16},
           {5400, 14, 0, 20},
           {5401, 14, 0, 24},
           {5402, 8, 0, 28},
           {5403, 8, 0, 32},
           {5405, 0, 0, 36},
           {5410, 0, 0, 37},
           {5411, 0, 0, 38},
           {5413, 0, 0, 39},
           {5408, 8, gff4::kList, 40},
           {5409, 8, gff4::kList, 44}}},
         {"ASLN",
          32,
          {{22, 17, 0, 0},
           {5300, 0, 0, 8},
           {5301, 8, 0, 12},
           {5302, 8, 0, 16},
           {5304, 0, 0, 20},
           {5562, 14, 0, 24},
           {5563, 14, 0, 28}}},
         {"ACTR",
          36,
          {{2, 14, 0, 0},
           {5201, 4, 0, 4},
           {5202, 14, 0, 8},
           {5206, kGeneric, gff4::kList | gff4::kReference, 12},
           {5216, 14, 0, 16},
           {5226, 8, 0, 20},
           {5214, 10, 0, 24}}},
         {"AJSC",
          20,
          {{5300, 0, 0, 0}, {5301, 8, 0, 4}, {5302, 8, 0, 8}, {1, 14, 0, 12}, {5570, 0, 0, 16}, {5571, 0, 0, 17}}}});
    const std::size_t root = Struct(b, 40);
    b.U32(root, b.String(u"bhm500ar_tower_harrowing"));
    Float(b, root + 4, 61.0F);
    b.U32(root + 8, b.String(u"gen00cs_cutscene_end"));
    b.U32(root + 12, b.String(u"bhmcs_intro.cub"));
    b.U32(root + 16, 30);
    b.U32(root + 20, 0xFFFFFFFF);
    b.U8(root + 24, 0);
    b.U8(root + 25, 255);
    b.U32(root + 28, b.String(u"gen00fl_alistair"));
    const std::uint32_t cub = b.String(u"bhmcs_intro.cub");
    b.U32(root + 32, b.Numbers({cub}));

    const std::size_t actors = b.bytes.size();
    b.U32(actors, 1);
    const std::size_t actor = Struct(b, 36);
    b.U32(root + 36, static_cast<std::uint32_t>(actors - b.data));
    b.U32(actor, b.String(u"Jowan"));
    b.U32(actor + 4, 7);
    b.U32(actor + 8, b.String(u"tools_null"));
    b.U32(actor + 16, b.String(u"bhm_jowan"));
    Float(b, actor + 20, 1.5F);
    Floats(b, actor + 24, {1.0F, 2.0F, 3.0F});

    const std::size_t animation = Struct(b, 48);
    b.U8(animation, 4);
    Float(b, animation + 4, 2.0F);
    Float(b, animation + 8, 1e10F);
    b.U8(animation + 16, 20);
    b.U32(animation + 20, b.String(u"mag_cast"));
    b.U32(animation + 24, b.String(u"bhmcs_intro.cub"));
    Float(b, animation + 28, 0.5F);
    Float(b, animation + 32, 0.25F);
    b.U8(animation + 36, 255);
    b.U8(animation + 38, 255);
    b.U32(animation + 40, FloatList(b, {0.0F, 0.0F, 0.0F, 0.0F, 0.5F, 1.0F, 2.0F, 3.0F}));
    b.U32(animation + 44, FloatList(b, {0.0F, 0.0F, 0.0F, 0.0F, 1.0F}));
    const std::size_t curves = b.bytes.size();
    b.U32(curves, 1);
    const std::size_t curve = Struct(b, 12);
    b.U32(animation + 12, static_cast<std::uint32_t>(curves - b.data));
    Float(b, curve, -6.5F);
    const std::size_t vertices = b.bytes.size();
    b.U32(vertices, 1);
    const std::size_t vertex = Struct(b, 8);
    b.U32(curve + 4, static_cast<std::uint32_t>(vertices - b.data));
    Floats(b, vertex, {41.5F, -20.25F});
    b.U32(curve + 8, b.Numbers({}));

    const std::size_t line = Struct(b, 32);
    b.U32(line, 6081234);
    b.U32(line + 4, 0);
    b.U8(line + 8, 23);
    Float(b, line + 12, 3.0F);
    Float(b, line + 16, 5.5F);
    b.U8(line + 20, 255);
    b.U32(line + 24, b.String(u"bhm_jowan_lip"));
    b.U32(line + 28, b.String(u"vo_bhm_jowan"));
    const std::size_t camera = Struct(b, 20);
    b.U8(camera, 24);
    Float(b, camera + 4, 0.0F);
    Float(b, camera + 8, 1e10F);
    b.U32(camera + 12, b.String(u"gen00fl_alistair"));
    b.U8(camera + 16, 0);
    b.U8(camera + 17, 255);
    b.U32(actor + 12, Children(b, {{4, animation}, {5, line}, {7, camera}}));
    return b.bytes;
}

} // namespace ghtest::levels
