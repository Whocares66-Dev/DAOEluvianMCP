// A model as glTF 2.0 (Gltf.h). The rules glTF holds a file to are its
// specification's (registry.khronos.org/glTF, 2.0): accessors of floats or
// unsigned integers each in a buffer view of its own, positions with their
// bounds, normals and tangents of length 1, a tangent's w its handedness,
// weights that sum to 1, rotations of length 1, triangles counter-clockwise
// seen from their front.
#include "ghilana/Gltf.h"

#include "ghilana/GltfMath.h"
#include "ghilana/Text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <span>

namespace eluvian::ghilana
{

namespace
{

namespace model = dao::core::model;
using namespace math; // NOLINT(google-build-using-namespace): the vectors and matrices of glTF's space

constexpr int kFloat = 5126;
constexpr int kUnsignedShort = 5123;
constexpr int kUnsignedInt = 5125;
constexpr int kVertices = 34962; // ARRAY_BUFFER
constexpr int kIndices = 34963;  // ELEMENT_ARRAY_BUFFER
constexpr int kTriangles = 4;

// The buffer and the JSON that points into it.
class Writer
{
  public:
    Json views = Json::array();
    Json accessors = Json::array();
    std::vector<std::byte> bytes;

    int View(std::span<const std::byte> data, std::optional<int> target)
    {
        bytes.resize((bytes.size() + 3) & ~std::size_t{3});
        Json view = {{"buffer", 0}, {"byteOffset", bytes.size()}, {"byteLength", data.size()}};
        if (target)
        {
            view["target"] = *target;
        }
        bytes.insert(bytes.end(), data.begin(), data.end());
        views.push_back(std::move(view));
        return static_cast<int>(views.size()) - 1;
    }

    // Floats, `width` a value: SCALAR, VEC2..4 or MAT4. With `bounds`, the
    // accessor's min and max, which glTF requires of positions.
    int Floats(const std::vector<float> &values, int width, std::optional<int> target, bool bounds = false)
    {
        const int view = View(std::as_bytes(std::span(values)), target);
        Json accessor = {{"bufferView", view},
                         {"componentType", kFloat},
                         {"count", values.size() / static_cast<std::size_t>(width)},
                         {"type", TypeName(width)}};
        if (bounds && !values.empty())
        {
            Json low = Json::array();
            Json high = Json::array();
            for (int c = 0; c < width; ++c)
            {
                float lo = std::numeric_limits<float>::max();
                float hi = std::numeric_limits<float>::lowest();
                for (auto i = static_cast<std::size_t>(c); i < values.size(); i += static_cast<std::size_t>(width))
                {
                    lo = std::min(lo, values[i]);
                    hi = std::max(hi, values[i]);
                }
                low.push_back(lo);
                high.push_back(hi);
            }
            accessor["min"] = std::move(low);
            accessor["max"] = std::move(high);
        }
        accessors.push_back(std::move(accessor));
        return static_cast<int>(accessors.size()) - 1;
    }

    int Shorts(const std::vector<std::uint16_t> &values, int width, std::optional<int> target)
    {
        const int view = View(std::as_bytes(std::span(values)), target);
        accessors.push_back(Json{{"bufferView", view},
                                 {"componentType", kUnsignedShort},
                                 {"count", values.size() / static_cast<std::size_t>(width)},
                                 {"type", TypeName(width)}});
        return static_cast<int>(accessors.size()) - 1;
    }

    int Ints(const std::vector<std::uint32_t> &values)
    {
        const int view = View(std::as_bytes(std::span(values)), kIndices);
        accessors.push_back(
            Json{{"bufferView", view}, {"componentType", kUnsignedInt}, {"count", values.size()}, {"type", "SCALAR"}});
        return static_cast<int>(accessors.size()) - 1;
    }

  private:
    static const char *TypeName(int width)
    {
        switch (width)
        {
        case 1:
            return "SCALAR";
        case 2:
            return "VEC2";
        case 3:
            return "VEC3";
        case 4:
            return "VEC4";
        default:
            return "MAT4";
        }
    }
};

// The model's skin: a glTF joint for each of its bones a node carries, in
// the bones' order.
struct Skin
{
    std::vector<int> joints; // the nodes
    std::vector<int> of;     // a bone index's joint, -1 for a bone no node carries

    // A blend index's joint: its slot in the mesh node's bones, that bone's joint.
    [[nodiscard]] std::optional<std::uint16_t> Joint(const std::vector<std::int32_t> &bonesUsed, int slot) const
    {
        if (slot < 0 || static_cast<std::size_t>(slot) >= bonesUsed.size())
        {
            return std::nullopt;
        }
        const std::int32_t bone = bonesUsed[static_cast<std::size_t>(slot)];
        if (bone < 0 || static_cast<std::size_t>(bone) >= of.size() || of[static_cast<std::size_t>(bone)] < 0)
        {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(of[static_cast<std::size_t>(bone)]);
    }
};

Skin SkinOf(const model::Hierarchy &tree)
{
    Skin skin;
    skin.of.assign(tree.bones.size(), -1);
    for (std::size_t bone = 0; bone < tree.bones.size(); ++bone)
    {
        if (tree.bones[bone] >= 0)
        {
            skin.of[bone] = static_cast<int>(skin.joints.size());
            skin.joints.push_back(tree.bones[bone]);
        }
    }
    return skin;
}

// One vertex's four joints and weights: each blend index made a joint, a
// weight on a bone no node carries dropped (counted in `dropped`) and the
// rest made to sum to 1; with no weight left, all of it on the mesh node's
// first bone.
std::pair<std::array<std::uint16_t, 4>, std::array<float, 4>> VertexJoints(const std::array<std::int16_t, 4> &bones,
                                                                           const model::Value &amounts,
                                                                           const std::vector<std::int32_t> &bonesUsed,
                                                                           const Skin &skin, std::size_t &dropped)
{
    std::array<std::uint16_t, 4> joint{};
    std::array<float, 4> weight{};
    float sum = 0;
    for (std::size_t k = 0; k < 4; ++k)
    {
        const float amount = std::max(amounts[k], 0.0F);
        const auto found = skin.Joint(bonesUsed, bones[k]);
        if (!found)
        {
            dropped += amount > 0 ? 1 : 0;
            continue;
        }
        joint[k] = amount > 0 ? *found : std::uint16_t{0};
        weight[k] = amount;
        sum += amount;
    }
    if (sum <= 0)
    {
        return {{skin.Joint(bonesUsed, 0).value_or(0), 0, 0, 0}, {1, 0, 0, 0}};
    }
    for (float &w : weight)
    {
        w /= sum;
    }
    return {joint, weight};
}

const model::Chunk *FindChunk(const model::Mesh &mesh, const std::wstring &name)
{
    for (const auto &chunk : mesh.chunks)
    {
        if (chunk.name == name)
        {
            return &chunk;
        }
    }
    const std::string key = Lower(Utf8(name));
    for (const auto &chunk : mesh.chunks)
    {
        if (Lower(Utf8(chunk.name)) == key)
        {
            return &chunk;
        }
    }
    return nullptr;
}

// One chunk as a glTF primitive's attributes and indices.
class ChunkWriter
{
  public:
    ChunkWriter(Writer &out, const model::Chunk &chunk, const std::string &label, std::vector<std::string> &notes)
        : out_(out), chunk_(chunk), label_(label), notes_(notes)
    {
    }

    // The primitive, or null where the chunk has no positions or is no
    // triangle list.
    Json Primitive(const model::MeshRef &ref, const Skin &skin, bool &skinned, bool &flipped)
    {
        if (chunk_.primitive != 0)
        {
            notes_.push_back(label_ + ": its primitive type is " + std::to_string(chunk_.primitive) +
                             ", not a triangle list; left out");
            return nullptr;
        }
        const model::Element *position = chunk_.Find(model::Usage::Position);
        const auto places = position != nullptr ? model::Floats(chunk_, *position) : std::vector<model::Value>{};
        if (places.empty())
        {
            notes_.push_back(label_ + ": no positions read; left out");
            return nullptr;
        }
        Json attributes = Json::object();
        std::vector<float> flat;
        flat.reserve(places.size() * 3);
        for (const auto &p : places)
        {
            const Vec3 v = Axes(p[0], p[1], p[2]);
            flat.insert(flat.end(), v.begin(), v.end());
        }
        attributes["POSITION"] = out_.Floats(flat, 3, kVertices, true);

        const std::vector<Vec3> normals = Normals(attributes);
        Tangents(normals, attributes);
        Sets(model::Usage::TexCoord, "TEXCOORD_", 2, attributes);
        Sets(model::Usage::Color, "COLOR_", 4, attributes);
        skinned = Joints(ref, skin, attributes);
        // Each vertex's place in the chunk. Blender splits a vertex where its
        // faces disagree on a UV or normal (a head's 2,282 came back 2,342,
        // 2026-10-14) and keeps a custom attribute on every copy, so the way
        // back joins the copies into the original layout a morph places by
        // (#139). A float holds every index of a chunk exactly.
        std::vector<float> order(places.size());
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            order[i] = static_cast<float>(i);
        }
        attributes[std::string(kOrderAttribute)] = out_.Floats(order, 1, kVertices);
        for (const auto &element : chunk_.declaration)
        {
            static constexpr std::array kTaken{
                model::Usage::Position, model::Usage::Normal, model::Usage::Tangent,     model::Usage::Binormal,
                model::Usage::TexCoord, model::Usage::Color,  model::Usage::BlendWeight, model::Usage::BlendIndices};
            if (std::ranges::find(kTaken, element.usage) == kTaken.end())
            {
                notes_.push_back(label_ + ": its " + UsageName(element.usage) + " " +
                                 std::to_string(element.usageIndex) + " has no place in glTF; left out");
            }
        }

        std::vector<std::uint32_t> indices = model::Indices(chunk_);
        indices.resize(indices.size() - (indices.size() % 3));
        flipped = Facing(flat, normals, indices) < 0;
        if (flipped)
        {
            for (std::size_t i = 0; i < indices.size(); i += 3)
            {
                std::swap(indices[i + 1], indices[i + 2]);
            }
        }
        Json primitive = {{"attributes", std::move(attributes)}, {"mode", kTriangles}};
        primitive["indices"] = Indices(indices);
        return primitive;
    }

  private:
    // NORMAL, of length 1; what was read, for the tangents and the winding.
    std::vector<Vec3> Normals(Json &attributes)
    {
        std::vector<Vec3> normals;
        const model::Element *element = chunk_.Find(model::Usage::Normal);
        if (element == nullptr)
        {
            return normals;
        }
        const auto read = model::Floats(chunk_, *element);
        if (read.size() != chunk_.vertexCount)
        {
            return normals;
        }
        std::vector<float> flat;
        flat.reserve(read.size() * 3);
        std::size_t zero = 0;
        for (const auto &n : read)
        {
            Vec3 v = Normalised(Axes(n[0], n[1], n[2]), {0, 0, 0});
            if (v == Vec3{0, 0, 0})
            {
                ++zero;
                v = {0, 1, 0};
            }
            normals.push_back(v);
            flat.insert(flat.end(), v.begin(), v.end());
        }
        if (zero > 0)
        {
            notes_.push_back(label_ + ": " + std::to_string(zero) + " normals of length 0, written as up");
        }
        attributes["NORMAL"] = out_.Floats(flat, 3, kVertices);
        return normals;
    }

    // TANGENT, its w the side the binormal is on: the file keeps tangent and
    // binormal both and no sign, and on the ogre's body the binormal is
    // cross(N, T) on some vertices and its mirror on others (lib-agent,
    // 2026-10-14, DAOLib #116).
    void Tangents(const std::vector<Vec3> &normals, Json &attributes)
    {
        const model::Element *tangent = chunk_.Find(model::Usage::Tangent);
        if (tangent == nullptr || normals.empty())
        {
            return;
        }
        const auto tangents = model::Floats(chunk_, *tangent);
        const model::Element *binormal = chunk_.Find(model::Usage::Binormal);
        const auto binormals = binormal != nullptr ? model::Floats(chunk_, *binormal) : std::vector<model::Value>{};
        if (tangents.size() != normals.size())
        {
            return;
        }
        std::vector<float> flat;
        flat.reserve(tangents.size() * 4);
        for (std::size_t i = 0; i < tangents.size(); ++i)
        {
            const Vec3 t = Normalised(Axes(tangents[i][0], tangents[i][1], tangents[i][2]), {1, 0, 0});
            float w = 1;
            if (i < binormals.size())
            {
                const Vec3 b = Axes(binormals[i][0], binormals[i][1], binormals[i][2]);
                w = Dot(Cross(normals[i], t), b) < 0 ? -1.0F : 1.0F;
            }
            flat.insert(flat.end(), {t[0], t[1], t[2], w});
        }
        attributes["TANGENT"] = out_.Floats(flat, 4, kVertices);
    }

    // Every set of a usage: TEXCOORD_n as two floats, COLOR_n as four.
    void Sets(model::Usage usage, const std::string &prefix, int width, Json &attributes)
    {
        int written = 0;
        for (std::uint8_t index = 0; index < 8; ++index)
        {
            const model::Element *element = chunk_.Find(usage, index);
            if (element == nullptr)
            {
                continue;
            }
            const auto read = model::Floats(chunk_, *element);
            if (read.size() != chunk_.vertexCount)
            {
                continue;
            }
            std::vector<float> flat;
            flat.reserve(read.size() * static_cast<std::size_t>(width));
            for (const auto &v : read)
            {
                flat.insert(flat.end(), v.begin(), v.begin() + width);
            }
            attributes[prefix + std::to_string(written++)] = out_.Floats(flat, width, kVertices);
        }
    }

    // JOINTS_0 and WEIGHTS_0: a vertex's blend indices count in its mesh
    // node's bones, which are the model's; a weight on a bone no node carries
    // is dropped and the rest made to sum to 1 again.
    bool Joints(const model::MeshRef &ref, const Skin &skin, Json &attributes)
    {
        const model::Element *indices = chunk_.Find(model::Usage::BlendIndices);
        const model::Element *weights = chunk_.Find(model::Usage::BlendWeight);
        if (indices == nullptr || weights == nullptr || ref.bonesUsed.empty())
        {
            return false;
        }
        const auto bones = model::Shorts(chunk_, *indices);
        const auto amounts = model::Floats(chunk_, *weights);
        if (bones.size() != chunk_.vertexCount || amounts.size() != chunk_.vertexCount)
        {
            notes_.push_back(label_ + ": its blend indices or weights could not be read; not skinned");
            return false;
        }
        std::vector<std::uint16_t> joints;
        std::vector<float> flat;
        joints.reserve(bones.size() * 4);
        flat.reserve(bones.size() * 4);
        std::size_t dropped = 0;
        for (std::size_t v = 0; v < bones.size(); ++v)
        {
            const auto [joint, weight] = VertexJoints(bones[v], amounts[v], ref.bonesUsed, skin, dropped);
            joints.insert(joints.end(), joint.begin(), joint.end());
            flat.insert(flat.end(), weight.begin(), weight.end());
        }
        if (dropped > 0)
        {
            notes_.push_back(label_ + ": " + std::to_string(dropped) +
                             " weights on bones no node of the model carries were dropped");
        }
        attributes["JOINTS_0"] = out_.Shorts(joints, 4, kVertices);
        attributes["WEIGHTS_0"] = out_.Floats(flat, 4, kVertices);
        return true;
    }

    // How the triangles face their vertices' normals: more facing them than
    // away, positive. glTF's front is counter-clockwise; a chunk whose
    // triangles mostly face away from their normals is wound the other way.
    static long long Facing(const std::vector<float> &places, const std::vector<Vec3> &normals,
                            const std::vector<std::uint32_t> &indices)
    {
        if (normals.empty())
        {
            return 0;
        }
        long long facing = 0;
        const std::size_t count = places.size() / 3;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            const std::uint32_t a = indices[i];
            const std::uint32_t b = indices[i + 1];
            const std::uint32_t c = indices[i + 2];
            if (a >= count || b >= count || c >= count)
            {
                continue;
            }
            const auto at = [&](std::uint32_t k) {
                return Vec3{places[k * 3], places[(k * 3) + 1], places[(k * 3) + 2]};
            };
            const Vec3 pa = at(a);
            const Vec3 pb = at(b);
            const Vec3 pc = at(c);
            const Vec3 face =
                Cross({pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]}, {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]});
            const Vec3 n = {normals[a][0] + normals[b][0] + normals[c][0],
                            normals[a][1] + normals[b][1] + normals[c][1],
                            normals[a][2] + normals[b][2] + normals[c][2]};
            const float d = Dot(face, n);
            if (d > 0)
            {
                ++facing;
            }
            else if (d < 0)
            {
                --facing;
            }
        }
        return facing;
    }

    int Indices(const std::vector<std::uint32_t> &indices)
    {
        const bool small = std::ranges::all_of(indices, [](std::uint32_t i) { return i <= 0xFFFF; });
        if (small)
        {
            const std::vector<std::uint16_t> narrow(indices.begin(), indices.end());
            const int view = out_.View(std::as_bytes(std::span(narrow)), kIndices);
            out_.accessors.push_back(Json{
                {"bufferView", view}, {"componentType", kUnsignedShort}, {"count", narrow.size()}, {"type", "SCALAR"}});
            return static_cast<int>(out_.accessors.size()) - 1;
        }
        return out_.Ints(indices);
    }

    Writer &out_;
    const model::Chunk &chunk_;
    const std::string &label_;
    std::vector<std::string> &notes_;
};

Json Declaration(const model::Chunk &chunk)
{
    Json out = Json::array();
    for (const auto &e : chunk.declaration)
    {
        out.push_back(Json{{"usage", UsageName(e.usage)},
                           {"index", e.usageIndex},
                           {"format", FormatName(e.format)},
                           {"offset", e.offset}});
    }
    return out;
}

Json Numbers(std::span<const float> values)
{
    Json out = Json::array();
    for (const float v : values)
    {
        out.push_back(v);
    }
    return out;
}

} // namespace

std::string UsageName(dao::core::model::Usage usage)
{
    static constexpr std::array<const char *, 14> kNames{
        "Position", "BlendWeight", "BlendIndices", "Normal", "PointSize", "TexCoord", "Tangent",
        "Binormal", "TessFactor",  "PositionT",    "Color",  "Fog",       "Depth",    "Sample"};
    const auto i = static_cast<std::size_t>(usage);
    return i < kNames.size() ? kNames[i] : "Usage" + std::to_string(i);
}

std::string FormatName(dao::core::model::Format format)
{
    static constexpr std::array<const char *, 17> kNames{
        "Float1",  "Float2",  "Float3",   "Float4",   "D3DColor", "UByte4", "Short2",    "Short4",   "UByte4N",
        "Short2N", "Short4N", "UShort2N", "UShort4N", "UDec3",    "Dec3N",  "Float16x2", "Float16x4"};
    const auto i = static_cast<std::size_t>(format);
    return i < kNames.size() ? kNames[i] : "Format" + std::to_string(i);
}

namespace
{

// The document made of a model: its nodes, then each mesh node's mesh, then
// the skin; the accessors' bytes in `writer_`.
class ModelWriter
{
  public:
    ModelWriter(const model::Hierarchy &tree, const model::Mesh &mesh, Gltf &out) : tree_(tree), mesh_(mesh), out_(out)
    {
    }

    void Write()
    {
        Nodes();
        const Skin skin = SkinOf(tree_);
        for (std::size_t i = 0; i < tree_.nodes.size(); ++i)
        {
            if (tree_.nodes[i].mesh)
            {
                MeshNode(i, skin);
            }
        }
        for (const auto &chunk : mesh_.chunks)
        {
            if (std::ranges::find(used_, &chunk) == used_.end())
            {
                out_.notes.push_back("chunk " + Utf8(chunk.name) + " is drawn by no mesh node; left out");
            }
        }
        Document(SkinJson(skin));
    }

  private:
    void Nodes()
    {
        world_.resize(tree_.nodes.size());
        for (std::size_t i = 0; i < tree_.nodes.size(); ++i)
        {
            const model::Node &node = tree_.nodes[i];
            const Vec3 t = Axes(node.translation[0], node.translation[1], node.translation[2]);
            const model::Quaternion q = Axes(node.rotation);
            const float s = node.scale.value_or(1.0F);
            const Mat4 local = Transform(t, q, s);
            world_[i] = node.parent >= 0 ? Times(world_[static_cast<std::size_t>(node.parent)], local) : local;
            Json item = {{"name", Utf8(node.name)}};
            if (!node.children.empty())
            {
                item["children"] = node.children;
            }
            if (t != Vec3{0, 0, 0})
            {
                item["translation"] = Numbers(t);
            }
            if (q != model::Quaternion{0, 0, 0, 1})
            {
                item["rotation"] = Numbers(q);
            }
            if (node.scale)
            {
                item["scale"] = Json::array({s, s, s});
            }
            Json dao = {{"kind", node.kind}};
            if (node.bone)
            {
                dao["bone"] = *node.bone;
            }
            item["extras"] = Json{{"dao", std::move(dao)}};
            nodes_.push_back(std::move(item));
            if (node.parent < 0)
            {
                scene_.push_back(i);
            }
        }
    }

    int Material(const std::wstring &name)
    {
        auto [at, added] = materialOf_.try_emplace(name, static_cast<int>(materials_.size()));
        if (added)
        {
            materials_.push_back(Json{{"name", Utf8(name)}, {"extras", {{"dao", {{"mao", Utf8(name) + ".mao"}}}}}});
        }
        return at->second;
    }

    void MeshNode(std::size_t i, const Skin &skin)
    {
        const model::Node &node = tree_.nodes[i];
        const model::MeshRef &ref = *node.mesh;
        const std::string label = "mesh node " + Utf8(node.name) + " (chunk " + Utf8(ref.chunk) + ")";
        const model::Chunk *chunk = FindChunk(mesh_, ref.chunk);
        if (chunk == nullptr)
        {
            out_.notes.push_back(label + ": no such chunk in " + Utf8(tree_.meshFile) + "; an empty node");
            return;
        }
        used_.push_back(chunk);
        bool skinned = false;
        bool flipped = false;
        ChunkWriter chunks(writer_, *chunk, label, out_.notes);
        Json primitive = chunks.Primitive(ref, skin, skinned, flipped);
        if (primitive.is_null())
        {
            return;
        }
        if (!ref.material.empty())
        {
            primitive["material"] = Material(ref.material);
        }
        // Its place among the chunks too: some meshes repeat a chunk's name
        // (fxa_ent_rt_c.msh's four "root"s; lib-agent, 2026-10-14).
        Json dao = {{"chunk", Utf8(chunk->name)},
                    {"chunk_index", chunk - mesh_.chunks.data()},
                    {"declaration", Declaration(*chunk)}};
        if (!ref.part.empty())
        {
            dao["part"] = Utf8(ref.part);
        }
        if (!ref.bonesUsed.empty())
        {
            dao["bones_used"] = ref.bonesUsed;
        }
        if (flipped)
        {
            // The file's triangles face away from their normals as glTF reads
            // them: turned here, and to be turned back on the way in. None of
            // the ogre's do (2026-10-14).
            dao["winding_turned"] = true;
        }
        meshes_.push_back(Json{{"name", Utf8(chunk->name)},
                               {"primitives", Json::array({std::move(primitive)})},
                               {"extras", {{"dao", std::move(dao)}}}});
        nodes_[i]["mesh"] = meshes_.size() - 1;
        if (skinned)
        {
            skinned_.push_back(i);
        }
    }

    // No bind pose is kept in the .mmh: its nodes' transforms are it
    // (lib-agent, 2026-10-14, read from the file; the ogre's body comes out
    // whole in Blender over it). The vertices are in their mesh node's space,
    // as a mesh node that is no skin's places its own (dao/core/Level.cpp), so
    // a joint's inverse bind matrix takes the mesh node's place in. glTF has a
    // skin's meshes at one place, the first's; another elsewhere is noted.
    Json SkinJson(const Skin &skin)
    {
        if (skinned_.empty())
        {
            return Json::array();
        }
        const Mat4 &meshPlace = world_[skinned_.front()];
        for (const std::size_t other : skinned_)
        {
            const bool same = std::ranges::equal(world_[other], meshPlace,
                                                 [](double a, double b) { return std::abs(a - b) <= 1e-4; });
            if (!same)
            {
                out_.notes.push_back("mesh node " + Utf8(tree_.nodes[other].name) +
                                     " is skinned at another place than the first; it is drawn at the first's");
            }
        }
        std::vector<float> inverse;
        inverse.reserve(skin.joints.size() * 16);
        for (const int joint : skin.joints)
        {
            const Mat4 m = Times(Inverse(world_[static_cast<std::size_t>(joint)]), meshPlace);
            for (const double v : m)
            {
                inverse.push_back(static_cast<float>(v));
            }
        }
        for (const std::size_t node : skinned_)
        {
            nodes_[node]["skin"] = 0;
        }
        return Json::array({Json{{"name", Utf8(tree_.name)},
                                 {"joints", skin.joints},
                                 {"inverseBindMatrices", writer_.Floats(inverse, 16, std::nullopt)}}});
    }

    void Document(Json skins)
    {
        Json &doc = out_.document;
        doc["asset"] = Json{{"version", "2.0"},
                            {"generator", "ghilana"},
                            {"extras",
                             {{"dao",
                               {{"model", Utf8(tree_.name)},
                                {"mesh", Utf8(tree_.meshFile)},
                                {"total_bones", tree_.totalBones},
                                {"up", "glTF's Y is the game's Z; the game's Y is glTF's -Z"}}}}}};
        doc["scene"] = 0;
        doc["scenes"] = Json::array({Json{{"name", Utf8(tree_.name)}, {"nodes", std::move(scene_)}}});
        doc["nodes"] = std::move(nodes_);
        Put(doc, "meshes", std::move(meshes_));
        Put(doc, "materials", std::move(materials_));
        Put(doc, "skins", std::move(skins));
        if (!writer_.accessors.empty())
        {
            doc["accessors"] = std::move(writer_.accessors);
            doc["bufferViews"] = std::move(writer_.views);
            writer_.bytes.resize((writer_.bytes.size() + 3) & ~std::size_t{3});
            doc["buffers"] = Json::array({Json{{"byteLength", writer_.bytes.size()}}});
        }
        out_.binary = std::move(writer_.bytes);
    }

    // A list put in the document if it has anything.
    static void Put(Json &doc, const char *key, Json list)
    {
        if (!list.empty())
        {
            doc[key] = std::move(list);
        }
    }

    const model::Hierarchy &tree_;
    const model::Mesh &mesh_;
    Gltf &out_;
    Writer writer_;
    Json nodes_ = Json::array();
    Json scene_ = Json::array();
    Json meshes_ = Json::array();
    Json materials_ = Json::array();
    std::vector<Mat4> world_;
    std::map<std::wstring, int> materialOf_;
    std::vector<const model::Chunk *> used_;
    std::vector<std::size_t> skinned_;
};

} // namespace

Gltf ModelGltf(const model::Hierarchy &tree, const model::Mesh &mesh)
{
    Gltf out;
    ModelWriter(tree, mesh, out).Write();
    return out;
}

} // namespace eluvian::ghilana
