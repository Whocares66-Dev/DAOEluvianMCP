// A model as glTF (ghilana/Gltf.h) and the tools over models (model,
// model_export), over the slab model DAOLib's tests read too
// (tests/model_builder.h): its Body chunk laid out as the ogre's, skinned
// over two of its three bones, and a Cap chunk no mesh node draws.
#include "Fixture.h"
#include "model_builder.h"

#include "ghilana/Gltf.h"
#include "ghilana/GltfImport.h"
#include "ghilana/GltfMath.h"
#include "ghilana/Text.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <dao/core/Gff4.h>

#include <cstring>
#include <fstream>
#include <iterator>

using namespace eluvian;
using namespace ghtest;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
namespace model = dao::core::model;

namespace
{

struct Slab
{
    std::vector<std::byte> meshBytes = SlabMesh();
    std::vector<std::byte> treeBytes = SlabModel();
    model::Mesh mesh;
    model::Hierarchy tree;

    Slab()
    {
        const auto meshFile = dao::core::gff4::File::Parse(meshBytes);
        const auto treeFile = dao::core::gff4::File::Parse(treeBytes);
        REQUIRE(meshFile);
        REQUIRE(treeFile);
        mesh = *model::MeshOf(*meshFile);
        tree = *model::HierarchyOf(*treeFile);
    }
};

// An accessor's floats, out of the glTF's buffer.
std::vector<float> Floats(const ghilana::Gltf &gltf, int accessor)
{
    const Json &a = gltf.document["accessors"][static_cast<std::size_t>(accessor)];
    REQUIRE(a["componentType"] == 5126);
    const Json &view = gltf.document["bufferViews"][a["bufferView"].get<std::size_t>()];
    std::vector<float> out(view["byteLength"].get<std::size_t>() / 4);
    std::memcpy(out.data(), gltf.binary.data() + view["byteOffset"].get<std::size_t>(), out.size() * 4);
    return out;
}

std::vector<std::uint16_t> Shorts(const ghilana::Gltf &gltf, int accessor)
{
    const Json &a = gltf.document["accessors"][static_cast<std::size_t>(accessor)];
    REQUIRE(a["componentType"] == 5123);
    const Json &view = gltf.document["bufferViews"][a["bufferView"].get<std::size_t>()];
    std::vector<std::uint16_t> out(view["byteLength"].get<std::size_t>() / 2);
    std::memcpy(out.data(), gltf.binary.data() + view["byteOffset"].get<std::size_t>(), out.size() * 2);
    return out;
}

bool Noted(const ghilana::Gltf &gltf, std::string_view text)
{
    return std::ranges::any_of(gltf.notes, [&](const std::string &n) { return n.find(text) != std::string::npos; });
}

} // namespace

TEST_CASE("glTF: the model's nodes in the file's order, Z up made Y up", "[ghilana][models]")
{
    const Slab slab;
    const ghilana::Gltf gltf = ghilana::ModelGltf(slab.tree, slab.mesh);
    const Json &doc = gltf.document;
    CHECK(doc["asset"]["version"] == "2.0");
    REQUIRE(doc["nodes"].size() == 4);
    CHECK(doc["scenes"][0]["nodes"] == Json::array({0}));
    const Json &gob = doc["nodes"][0];
    CHECK(gob["name"] == "GOB");
    CHECK(gob["children"] == Json::array({1, 3}));
    // Moved +z in the game: +y in glTF.
    CHECK(gob["translation"] == Json::array({0.0F, 1.0F, -0.0F}));
    CHECK(gob["extras"]["dao"]["kind"] == "node");
    const Json &bone0 = doc["nodes"][1];
    CHECK(bone0["extras"]["dao"]["bone"] == 1);
    // A quarter turn about the game's z is one about glTF's y.
    CHECK(bone0["rotation"][1].get<float>() == Approx(0.70710678F));
    CHECK(bone0["rotation"][3].get<float>() == Approx(0.70710678F));
    CHECK(bone0["scale"] == Json::array({2.0F, 2.0F, 2.0F}));
    // Moved +y in the game: -z in glTF.
    CHECK(doc["nodes"][2]["translation"][2].get<float>() == Approx(-1));
}

TEST_CASE("glTF: a skinned chunk's every element, its blend indices made the model's joints", "[ghilana][models]")
{
    const Slab slab;
    const ghilana::Gltf gltf = ghilana::ModelGltf(slab.tree, slab.mesh);
    const Json &doc = gltf.document;
    REQUIRE(doc["meshes"].size() == 1);
    CHECK(doc["nodes"][3]["mesh"] == 0);
    CHECK(doc["nodes"][3]["skin"] == 0);
    const Json &mesh = doc["meshes"][0];
    CHECK(mesh["extras"]["dao"]["chunk"] == "Body");
    CHECK(mesh["extras"]["dao"]["part"] == "_1");
    CHECK(mesh["extras"]["dao"]["bones_used"] == Json::array({1, 0}));
    CHECK(mesh["extras"]["dao"]["declaration"].size() == 7);
    CHECK(doc["materials"][0]["name"] == "ogre");
    const Json &primitive = mesh["primitives"][0];
    CHECK(primitive["material"] == 0);
    const Json &attributes = primitive["attributes"];
    for (const char *name : {"POSITION", "NORMAL", "TANGENT", "TEXCOORD_0", "JOINTS_0", "WEIGHTS_0"})
    {
        CHECK(attributes.contains(name));
    }

    const auto places = Floats(gltf, attributes["POSITION"]);
    REQUIRE(places.size() == 9);
    CHECK(places[6] == Approx(2));
    const Json &positions = doc["accessors"][attributes["POSITION"].get<std::size_t>()];
    CHECK(positions["max"][0].get<float>() == Approx(2));
    // The game's normal (0, 0, -1) is glTF's (0, -1, 0).
    const auto normals = Floats(gltf, attributes["NORMAL"]);
    CHECK(normals[1] == Approx(-1));
    // The binormal is cross(N, T): the tangent's w is 1.
    const auto tangents = Floats(gltf, attributes["TANGENT"]);
    CHECK(tangents[0] == Approx(1));
    CHECK(tangents[3] == Approx(1));
    const auto uv = Floats(gltf, attributes["TEXCOORD_0"]);
    CHECK(uv[2] == Approx(1)); // the second vertex's u

    // The model's bones are [Bone1, Bone0, none]: joint 0 is Bone1's node (2),
    // joint 1 Bone0's (1). Vertex 0's blend indices (0, 1, 2, 0) with weights
    // (0.5, 0.25, 0.25, 0) count in the mesh node's bones [1, 0]: slot 0 is
    // bone 1, joint 1; slot 1 bone 0, joint 0; slot 2 is past them, dropped.
    CHECK(doc["skins"][0]["joints"] == Json::array({2, 1}));
    const auto joints = Shorts(gltf, attributes["JOINTS_0"]);
    const auto weights = Floats(gltf, attributes["WEIGHTS_0"]);
    REQUIRE(joints.size() == 12);
    CHECK(joints[0] == 1);
    CHECK(joints[1] == 0);
    CHECK(weights[0] == Approx(0.5F / 0.75F));
    CHECK(weights[1] == Approx(0.25F / 0.75F));
    CHECK(weights[2] == 0);
    CHECK(Noted(gltf, "weights on bones no node"));
    CHECK(Noted(gltf, "chunk Cap is drawn by no mesh node"));
}

TEST_CASE("glTF: a bone no node carries is no joint, and the joints after it count on", "[ghilana][models]")
{
    // Bone 0 (Bone1's) without its node: Bone0, bone 1, is joint 0, and
    // vertex 0's slot 0, bone 1, is joint 0 and not 1.
    Slab slab;
    slab.tree.bones[0] = -1;
    const ghilana::Gltf gltf = ghilana::ModelGltf(slab.tree, slab.mesh);
    const Json &doc = gltf.document;
    CHECK(doc["skins"][0]["joints"] == Json::array({1}));
    const auto joints = Shorts(gltf, doc["meshes"][0]["primitives"][0]["attributes"]["JOINTS_0"]);
    REQUIRE(!joints.empty());
    CHECK(joints[0] == 0);
}

TEST_CASE("glTF: the game's axes turned to glTF's and back are the game's again", "[ghilana][models]")
{
    const ghilana::math::Vec3 v = ghilana::math::Back(ghilana::math::Axes(1, 2, 3));
    CHECK(v[0] == 1);
    CHECK(v[1] == 2);
    CHECK(v[2] == 3);
}

TEST_CASE("glTF: a joint's inverse bind matrix takes the mesh node's place to the joint's", "[ghilana][models]")
{
    const Slab slab;
    const ghilana::Gltf gltf = ghilana::ModelGltf(slab.tree, slab.mesh);
    const auto inverse = Floats(gltf, gltf.document["skins"][0]["inverseBindMatrices"]);
    REQUIRE(inverse.size() == 32);
    // Joint 0 is Bone1: in glTF's axes at GOB (0, 1, 0), turned a quarter
    // about y and scaled 2 by Bone0, then moved (0, 0, -1): at (-2, 1, 0).
    // The mesh node is at GOB's (0, 1, 0), which in Bone1's space is half of
    // (2, 0, 0) turned back a quarter about y: (0, 0, 1).
    CHECK(inverse[12] == Approx(0).margin(1e-5));
    CHECK(inverse[13] == Approx(0).margin(1e-5));
    CHECK(inverse[14] == Approx(1));
    CHECK(inverse[0] == Approx(0).margin(1e-5)); // the turn back
    CHECK(inverse[2] == Approx(0.5F));           // and the scale's
}

TEST_CASE("model: what a model is made of; model_export: it as glTF beside a .bin", "[ghilana][models][tools]")
{
    Served served;
    served.install.root.Write(L"documents/packages/core/override/slab.mmh", SlabModel());
    served.install.root.Write(L"documents/packages/core/override/slab.msh", SlabMesh());

    const Json m = served.Call("model", {{"name", "slab"}, {"tree", true}});
    CHECK(m["model"]["name"] == "slab.mmh");
    CHECK(m["mesh_file"]["name"] == "slab.msh");
    CHECK(m["nodes"] == 4);
    CHECK(m["kinds"]["mshh"] == 1);
    CHECK(m["bones"]["total"] == 3);
    CHECK(m["bones"]["with_a_node"] == 2);
    REQUIRE(m["meshes"].size() == 1);
    CHECK(m["meshes"][0]["chunk"] == "Body");
    CHECK(m["meshes"][0]["vertices"] == 3);
    CHECK(m["meshes"][0]["skinned"] == true);
    CHECK(m["meshes"][0]["elements"][0] == "Position 0 Float16x4");
    CHECK(m["tree"].size() == 4);

    const auto to = served.install.root.path / L"out";
    const Json e = served.Call("model_export", {{"name", "slab.mmh"}, {"to", ghilana::PathText(to)}});
    CHECK(e["meshes"] == 1);
    CHECK(e["joints"] == 2);
    REQUIRE(std::filesystem::exists(to / L"slab.gltf"));
    REQUIRE(std::filesystem::exists(to / L"slab.bin"));
    std::ifstream in(to / L"slab.gltf");
    const Json doc = Json::parse(in);
    CHECK(doc["buffers"][0]["uri"] == "slab.bin");
    CHECK(doc["buffers"][0]["byteLength"].get<std::uintmax_t>() == std::filesystem::file_size(to / L"slab.bin"));

    const auto refused = [&](const Json &args) {
        try
        {
            served.Call("model_export", args);
        }
        catch (const ToolError &error)
        {
            return std::string(error.what());
        }
        return std::string();
    };
    CHECK_THAT(refused({{"name", "slab.mmh"}, {"to", ghilana::PathText(served.install.game / L"x")}}),
               ContainsSubstring("never writes"));
    CHECK_THAT(refused({{"name", "slab.msh"}, {"to", ghilana::PathText(to)}}), ContainsSubstring(".mmh"));
}

namespace
{

// A model's mesh as written into a folder, read back.
struct WrittenMesh
{
    std::vector<std::byte> bytes;
    model::Mesh mesh;

    explicit WrittenMesh(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        bytes.resize(chars.size());
        std::memcpy(bytes.data(), chars.data(), chars.size());
        const auto file = dao::core::gff4::File::Parse(bytes);
        REQUIRE(file);
        const auto read = model::MeshOf(*file);
        REQUIRE(read);
        mesh = *read;
    }
};

std::string ImportRefusal(Served &served, const Json &args)
{
    try
    {
        served.Call("model_import", args);
    }
    catch (const ToolError &error)
    {
        return error.what();
    }
    return {};
}

} // namespace

TEST_CASE("model_import: a model's own glTF comes back as it was; an edit in it comes back too", "[ghilana][models]")
{
    Served served;
    served.install.root.Write(L"documents/packages/core/override/slab.mmh", SlabModel());
    served.install.root.Write(L"documents/packages/core/override/slab.msh", SlabMesh());
    const auto out = served.install.root.path / L"out";
    served.Call("model_export", {{"name", "slab"}, {"to", ghilana::PathText(out)}});
    const std::string gltf = ghilana::PathText(out / L"slab.gltf");

    // Unchanged: the same vertices, triangles and bones, every element.
    const auto back = served.install.root.path / L"back";
    Json answer = served.Call("model_import", {{"name", "slab"}, {"gltf", gltf}, {"to", ghilana::PathText(back)}});
    CHECK(answer["chunks"][0]["vertices"]["after"] == 3);
    CHECK(answer["node_edits"].empty());
    CHECK_THAT(answer["notes"].dump(), ContainsSubstring("chunk Cap is in no mesh of the glTF; kept as it was"));
    const Slab slab;
    const WrittenMesh written(back / L"slab.msh");
    REQUIRE(written.mesh.chunks.size() == 2);
    const model::Chunk &before = slab.mesh.chunks[0];
    const model::Chunk &after = written.mesh.chunks[0];
    // Every element but the bones' (below): the slab weights some vertices to
    // a slot past its mesh node's bones, which the export drops.
    for (const auto &element : before.declaration)
    {
        const bool bones = element.usage == model::Usage::BlendIndices || element.usage == model::Usage::BlendWeight;
        const auto a = bones ? std::vector<model::Value>{} : model::Floats(before, element);
        const auto b = bones ? std::vector<model::Value>{} : model::Floats(after, *after.Find(element.usage));
        REQUIRE(a.size() == b.size());
        for (std::size_t v = 0; v < a.size(); ++v)
        {
            for (std::size_t c = 0; c < 4; ++c)
            {
                CHECK(b[v][c] == Approx(a[v][c]).margin(1e-3));
            }
        }
    }
    CHECK(model::Indices(after) == model::Indices(before));
    // Vertex 0's bones: slot 0 (bone 1) and slot 1 (bone 0), as the export
    // made them joints; the slot past the mesh node's list was dropped then.
    const auto bones = model::Shorts(after, *after.Find(model::Usage::BlendIndices));
    CHECK(bones[0][0] == 0);
    CHECK(bones[0][1] == 1);
    const auto weights = model::Floats(after, *after.Find(model::Usage::BlendWeight));
    CHECK(weights[0][0] == Approx(2.0 / 3).margin(1e-3));
    CHECK(weights[0][1] == Approx(1.0 / 3).margin(1e-3));

    // An edit: every place twice as far from the mesh node.
    const Json doc = Json::parse(std::ifstream(out / L"slab.gltf"));
    const Json &accessor =
        doc["accessors"][doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"].get<std::size_t>()];
    const Json &view = doc["bufferViews"][accessor["bufferView"].get<std::size_t>()];
    std::fstream bin(out / L"slab.bin", std::ios::in | std::ios::out | std::ios::binary);
    for (std::size_t i = 0; i < 9; ++i)
    {
        const std::size_t offset = view["byteOffset"].get<std::size_t>() + (4 * i);
        const auto at = static_cast<std::streamoff>(offset);
        float value = 0;
        bin.seekg(at);
        bin.read(reinterpret_cast<char *>(&value), 4);
        value *= 2;
        bin.seekp(at);
        bin.write(reinterpret_cast<const char *>(&value), 4);
    }
    bin.close();
    const auto edited = served.install.root.path / L"edited";
    served.Call("model_import", {{"name", "slab"}, {"gltf", gltf}, {"to", ghilana::PathText(edited)}});
    const WrittenMesh moved(edited / L"slab.msh");
    const auto places = model::Floats(moved.mesh.chunks[0], *moved.mesh.chunks[0].Find(model::Usage::Position));
    CHECK(places[2][0] == Approx(4));
}

TEST_CASE("model_import: a mesh copied in the glTF is a new chunk under a new mesh node like its own",
          "[ghilana][models]")
{
    Served served;
    served.install.root.Write(L"documents/packages/core/override/slab.mmh", SlabModel());
    served.install.root.Write(L"documents/packages/core/override/slab.msh", SlabMesh());
    const auto out = served.install.root.path / L"out";
    served.Call("model_export", {{"name", "slab"}, {"to", ghilana::PathText(out)}});
    // Blender's duplicate: a second node of the same mesh, its extras and all,
    // with another material.
    Json doc = Json::parse(std::ifstream(out / L"slab.gltf"));
    Json copy = doc["nodes"][3];
    copy["name"] = "Mesh1.001";
    copy.erase("children");
    doc["nodes"].push_back(copy);
    doc["scenes"][0]["nodes"].push_back(doc["nodes"].size() - 1);
    doc["meshes"].push_back(doc["meshes"][0]);
    doc["meshes"].back()["name"] = "Body.001";
    doc["meshes"].back()["primitives"][0]["material"] = doc["materials"].size();
    doc["materials"].push_back({{"name", "troll"}});
    doc["nodes"].back()["mesh"] = doc["meshes"].size() - 1;
    // The copy's own skin, both its joints Bone1 (node 2, bone 0): its
    // vertices weigh on bone 0 alone.
    Json skin = doc["skins"][0];
    skin["joints"] = Json::array({2, 2});
    doc["skins"].push_back(skin);
    doc["nodes"].back()["skin"] = doc["skins"].size() - 1;
    std::ofstream(out / L"slab.gltf") << doc.dump();

    const auto back = served.install.root.path / L"back";
    const Json answer = served.Call(
        "model_import",
        {{"name", "slab"}, {"gltf", ghilana::PathText(out / L"slab.gltf")}, {"to", ghilana::PathText(back)}});
    REQUIRE(answer["nodes_added"].size() == 1);
    CHECK(answer["nodes_added"][0]["node"] == "Mesh1_001");
    CHECK(answer["nodes_added"][0]["chunk"] == "Body_001");
    CHECK(answer["nodes_added"][0]["material"] == "troll");
    CHECK(answer["nodes_added"][0]["like"] == "Mesh1");
    CHECK(answer["chunks"][1]["added"] == true);

    const WrittenMesh written(back / L"slab.msh");
    REQUIRE(written.mesh.chunks.size() == 3);
    const model::Chunk &body = written.mesh.chunks[0];
    const model::Chunk &added = written.mesh.chunks[2];
    CHECK(added.name == L"Body_001");
    CHECK(added.vertexCount == body.vertexCount);
    CHECK(model::Indices(added) == model::Indices(body));
    CHECK(added.declaration.size() == body.declaration.size());

    std::ifstream in(back / L"slab.mmh", std::ios::binary);
    const std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::byte> treeBytes(chars.size());
    std::memcpy(treeBytes.data(), chars.data(), chars.size());
    const auto treeFile = dao::core::gff4::File::Parse(treeBytes);
    REQUIRE(treeFile);
    const auto tree = model::HierarchyOf(*treeFile);
    REQUIRE(tree);
    const auto node = std::ranges::find(tree->nodes, std::wstring(L"Mesh1_001"), &model::Node::name);
    REQUIRE(node != tree->nodes.end());
    REQUIRE(node->mesh);
    CHECK(node->mesh->chunk == L"Body_001");
    CHECK(node->mesh->material == L"troll");
    // Its own bones only, not the original's [1, 0].
    CHECK(node->mesh->bonesUsed == std::vector<std::int32_t>{0});
}

TEST_CASE("a chunk over too many bones is split, each triangle in the first piece its bones fit", "[ghilana][models]")
{
    // Bones [10, 11, 12] and at most two a piece. Vertices 0 and 2 on bone 10,
    // 1 on 11, 3 on 12 (slot 2) with a weightless slot 1 left behind.
    model::ChunkData whole;
    whole.name = L"Body";
    whole.index = 0;
    std::vector<model::Element> declaration(3);
    declaration[1].usage = model::Usage::BlendIndices;
    declaration[2].usage = model::Usage::BlendWeight;
    whole.values = {{{0, 0, 0, 1}, {1, 0, 0, 1}, {2, 0, 0, 1}, {3, 0, 0, 1}},
                    {{0, 0, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}, {2, 1, 0, 0}},
                    {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}}};
    // A on bones 10 and 11; B on 10 and 12, which A's piece cannot take; C as A.
    whole.indices = {0, 1, 2, 0, 2, 3, 1, 2, 0};
    const auto pieces = ghilana::SplitByBones(whole, declaration, {10, 11, 12}, 2);
    REQUIRE(pieces.size() == 2);
    CHECK(pieces[0].bones == std::vector<std::int32_t>{10, 11});
    CHECK(pieces[1].bones == std::vector<std::int32_t>{10, 12});
    CHECK(pieces[0].data.name == L"Body");
    CHECK(pieces[0].data.index == 0);
    // A and C in the first piece, over its own three vertices.
    CHECK(pieces[0].data.indices == std::vector<std::uint32_t>{0, 1, 2, 1, 2, 0});
    REQUIRE(pieces[0].data.values[0].size() == 3);
    CHECK(pieces[0].data.values[1][1][0] == 1); // vertex 1, bone 11, slot 1 here
    // B in the second: vertices 0, 2 and 3 again, vertex 3's bone 12 at slot 1.
    CHECK(pieces[1].data.indices == std::vector<std::uint32_t>{0, 1, 2});
    REQUIRE(pieces[1].data.values[0].size() == 3);
    CHECK(pieces[1].data.values[0][2][0] == 3); // its place came along
    CHECK(pieces[1].data.values[1][2][0] == 1);
    CHECK(pieces[1].data.values[1][0][0] == 0); // vertex 0, bone 10, slot 0
}

TEST_CASE("model_import: a joint that is no bone of the model is a new bone under its parent", "[ghilana][models]")
{
    Served served;
    served.install.root.Write(L"documents/packages/core/override/slab.mmh", SlabModel());
    served.install.root.Write(L"documents/packages/core/override/slab.msh", SlabMesh());
    const auto out = served.install.root.path / L"out";
    served.Call("model_export", {{"name", "slab"}, {"to", ghilana::PathText(out)}});
    // Bone1 (joint 0, under Bone0) renamed: to the model, a joint it has not.
    Json doc = Json::parse(std::ifstream(out / L"slab.gltf"));
    doc["nodes"][2]["name"] = "Nope";
    std::ofstream(out / L"slab.gltf") << doc.dump();
    const auto back = served.install.root.path / L"back";
    const Json answer = served.Call(
        "model_import",
        {{"name", "slab"}, {"gltf", ghilana::PathText(out / L"slab.gltf")}, {"to", ghilana::PathText(back)}});
    REQUIRE(answer["nodes_added"].size() == 1);
    CHECK(answer["nodes_added"][0]["node"] == "Nope");
    CHECK(answer["nodes_added"][0]["bone"] == true);
    CHECK(answer["nodes_added"][0]["parent"] == "Bone0");
    CHECK(answer["nodes_added"][0]["like"] == "Bone0");

    std::ifstream in(back / L"slab.mmh", std::ios::binary);
    const std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::byte> treeBytes(chars.size());
    std::memcpy(treeBytes.data(), chars.data(), chars.size());
    const auto treeFile = dao::core::gff4::File::Parse(treeBytes);
    REQUIRE(treeFile);
    const auto tree = model::HierarchyOf(*treeFile);
    REQUIRE(tree);
    const auto nope = std::ranges::find(tree->nodes, std::wstring(L"Nope"), &model::Node::name);
    REQUIRE(nope != tree->nodes.end());
    // The bone after the model's three, where Bone1 is: (0, 1, 0) from Bone0, not turned.
    CHECK(nope->bone == 3);
    CHECK(tree->nodes[static_cast<std::size_t>(nope->parent)].name == L"Bone0");
    CHECK(nope->translation[0] == Approx(0).margin(1e-4));
    CHECK(nope->translation[1] == Approx(1).margin(1e-4));
    CHECK(nope->translation[2] == Approx(0).margin(1e-4));
    CHECK(nope->rotation[3] == Approx(1).margin(1e-4));
    // The mesh node counts in it: vertex 0's joint 0 is bone 3 now.
    const auto mesh = std::ranges::find(tree->nodes, std::wstring(L"Mesh1"), &model::Node::name);
    REQUIRE(mesh != tree->nodes.end());
    REQUIRE(mesh->mesh);
    CHECK(std::ranges::find(mesh->mesh->bonesUsed, 3) != mesh->mesh->bonesUsed.end());

    CHECK_THAT(ImportRefusal(served, {{"name", "slab"},
                                      {"gltf", ghilana::PathText(out / L"none.gltf")},
                                      {"to", ghilana::PathText(out / L"x")}}),
               ContainsSubstring("cannot open"));
}
