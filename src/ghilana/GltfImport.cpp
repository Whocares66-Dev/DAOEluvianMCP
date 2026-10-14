// A model back from glTF (GltfImport.h). glTF's rules are its
// specification's (registry.khronos.org/glTF, 2.0): a .glb is a header and a
// JSON and a BIN chunk; an accessor reads its buffer view at a stride, its
// integers normalised when it says so; a node has a matrix or a translation,
// rotation and scale; a skinned vertex is placed by its joints' world
// matrices times their inverse bind matrices, weighted.
#include "ghilana/GltfImport.h"

#include "ghilana/Gltf.h"
#include "ghilana/GltfMath.h"
#include "ghilana/Text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>

namespace eluvian::ghilana
{

namespace
{

namespace model = dao::core::model;
using namespace math; // NOLINT(google-build-using-namespace): the vectors and matrices of glTF's space
using Value = model::Value;

std::vector<std::byte> ReadBytes(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error("cannot open " + PathText(path));
    }
    std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

std::uint32_t U32(const std::vector<std::byte> &bytes, std::size_t at)
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

// A uri's %XX made bytes again: Blender writes a .bin's name as a uri.
std::string Unescaped(std::string_view uri)
{
    std::string out;
    for (std::size_t i = 0; i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size())
        {
            out.push_back(static_cast<char>(std::stoi(std::string(uri.substr(i + 1, 2)), nullptr, 16)));
            i += 2;
        }
        else
        {
            out.push_back(uri[i]);
        }
    }
    return out;
}

// --------------------------------------------------------------- accessors

std::size_t Width(const std::string &type)
{
    static const std::map<std::string, std::size_t> kWidths{{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4},
                                                            {"MAT2", 4},   {"MAT3", 9}, {"MAT4", 16}};
    const auto found = kWidths.find(type);
    if (found == kWidths.end())
    {
        throw std::runtime_error("an accessor of type \"" + type + "\", which glTF has no such");
    }
    return found->second;
}

std::size_t ComponentSize(int type)
{
    switch (type)
    {
    case 5120:
    case 5121:
        return 1;
    case 5122:
    case 5123:
        return 2;
    case 5125:
    case 5126:
        return 4;
    default:
        throw std::runtime_error("an accessor of component type " + std::to_string(type) + ", which glTF has no such");
    }
}

double Component(const std::byte *at, int type, bool normalised)
{
    switch (type)
    {
    case 5120: {
        const auto v = static_cast<std::int8_t>(*at);
        return normalised ? std::max(v / 127.0, -1.0) : v;
    }
    case 5121: {
        const auto v = static_cast<std::uint8_t>(*at);
        return normalised ? v / 255.0 : v;
    }
    case 5122: {
        std::int16_t v = 0;
        std::memcpy(&v, at, 2);
        return normalised ? std::max(v / 32767.0, -1.0) : v;
    }
    case 5123: {
        std::uint16_t v = 0;
        std::memcpy(&v, at, 2);
        return normalised ? v / 65535.0 : v;
    }
    case 5125: {
        std::uint32_t v = 0;
        std::memcpy(&v, at, 4);
        return v;
    }
    default: {
        float v = 0;
        std::memcpy(&v, at, 4);
        return v;
    }
    }
}

// An accessor's values, `width` numbers each, flat.
std::vector<double> Numbers(const GltfFile &gltf, std::size_t index, std::size_t &width)
{
    const Json &accessors = gltf.document.value("accessors", Json::array());
    if (index >= accessors.size())
    {
        throw std::runtime_error("accessor " + std::to_string(index) + " is not in the file");
    }
    const Json &a = accessors[index];
    if (a.contains("sparse"))
    {
        throw std::runtime_error("accessor " + std::to_string(index) +
                                 " is sparse, which is not read; export again "
                                 "without sparse accessors");
    }
    width = Width(a.value("type", std::string()));
    const auto count = a.value("count", std::size_t{0});
    std::vector<double> out(count * width, 0.0);
    if (!a.contains("bufferView"))
    {
        return out; // all zeros, as glTF says
    }
    const int type = a.value("componentType", 5126);
    const bool normalised = a.value("normalized", false);
    const Json &view = gltf.document["bufferViews"].at(a["bufferView"].get<std::size_t>());
    const auto buffer = view.value("buffer", std::size_t{0});
    if (buffer >= gltf.buffers.size())
    {
        throw std::runtime_error("a buffer view names buffer " + std::to_string(buffer) + ", which is not read");
    }
    const std::vector<std::byte> &bytes = gltf.buffers[buffer];
    const std::size_t size = ComponentSize(type);
    const std::size_t stride = view.value("byteStride", size * width);
    const std::size_t start = view.value("byteOffset", std::size_t{0}) + a.value("byteOffset", std::size_t{0});
    if (count > 0 && start + (stride * (count - 1)) + (size * width) > bytes.size())
    {
        throw std::runtime_error("accessor " + std::to_string(index) + " runs past its buffer");
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        for (std::size_t c = 0; c < width; ++c)
        {
            out[(i * width) + c] = Component(bytes.data() + start + (stride * i) + (size * c), type, normalised);
        }
    }
    return out;
}

// An accessor of up to four numbers a value, each as four floats; the rest
// filled as (0, 0, 0, 1).
std::vector<Value> Values(const GltfFile &gltf, std::size_t index)
{
    std::size_t width = 0;
    const auto flat = Numbers(gltf, index, width);
    if (width > 4)
    {
        throw std::runtime_error("accessor " + std::to_string(index) + " is a matrix where a vector was wanted");
    }
    std::vector<Value> out(width == 0 ? 0 : flat.size() / width, Value{0, 0, 0, 1});
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        for (std::size_t c = 0; c < width; ++c)
        {
            out[i][c] = static_cast<float>(flat[(i * width) + c]);
        }
    }
    return out;
}

std::vector<Mat4> Matrices(const GltfFile &gltf, std::size_t index)
{
    std::size_t width = 0;
    const auto flat = Numbers(gltf, index, width);
    if (width != 16)
    {
        throw std::runtime_error("accessor " + std::to_string(index) + " is no MAT4");
    }
    std::vector<Mat4> out(flat.size() / 16);
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        std::copy_n(flat.begin() + static_cast<std::ptrdiff_t>(i * 16), 16, out[i].begin());
    }
    return out;
}

// --------------------------------------------------------------- the scene

Mat4 NodeLocal(const Json &node)
{
    if (node.contains("matrix"))
    {
        Mat4 m{};
        for (std::size_t k = 0; k < 16; ++k)
        {
            m[k] = node["matrix"].at(k).get<double>();
        }
        return m;
    }
    const auto t = node.value("translation", std::vector<float>{0, 0, 0});
    const auto r = node.value("rotation", std::vector<float>{0, 0, 0, 1});
    const auto s = node.value("scale", std::vector<float>{1, 1, 1});
    Mat4 m = Transform({t.at(0), t.at(1), t.at(2)}, {r.at(0), r.at(1), r.at(2), r.at(3)}, 1.0F);
    for (std::size_t col = 0; col < 3; ++col)
    {
        for (std::size_t row = 0; row < 3; ++row)
        {
            m[(col * 4) + row] *= s.at(col);
        }
    }
    return m;
}

// Every node's world matrix: its parents', outward, and its own.
std::vector<Mat4> SceneWorlds(const Json &nodes)
{
    std::vector<int> parent(nodes.size(), -1);
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        for (const auto &child : nodes[i].value("children", Json::array()))
        {
            const auto c = child.get<std::size_t>();
            if (c < parent.size())
            {
                parent[c] = static_cast<int>(i);
            }
        }
    }
    std::vector<std::optional<Mat4>> world(nodes.size());
    std::function<Mat4(std::size_t, int)> of = [&](std::size_t i, int depth) -> Mat4 {
        if (!world[i])
        {
            const Mat4 local = NodeLocal(nodes[i]);
            world[i] = parent[i] >= 0 && depth < 1000 ? Times(of(static_cast<std::size_t>(parent[i]), depth + 1), local)
                                                      : local;
        }
        return *world[i];
    };
    std::vector<Mat4> out;
    out.reserve(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        out.push_back(of(i, 0));
    }
    return out;
}

// A name as Blender may have changed it: "Bone.001" for a second "Bone".
std::string Unnumbered(const std::string &name)
{
    if (name.size() > 4 && name[name.size() - 4] == '.' &&
        std::all_of(name.end() - 3, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
    {
        return name.substr(0, name.size() - 4);
    }
    return name;
}

// The model's nodes by name, in lower case.
std::multimap<std::string, int> NodesByName(const model::Hierarchy &tree)
{
    std::multimap<std::string, int> out;
    for (std::size_t i = 0; i < tree.nodes.size(); ++i)
    {
        out.emplace(Lower(Utf8(tree.nodes[i].name)), static_cast<int>(i));
    }
    return out;
}

// The model's node of a name that `wanted` says yes to, the name as given or without Blender's number.
std::optional<int> NodeNamed(const std::multimap<std::string, int> &byName, const std::string &name,
                             const std::function<bool(int)> &wanted)
{
    for (const std::string &key : {Lower(name), Lower(Unnumbered(name))})
    {
        const auto [from, to] = byName.equal_range(key);
        for (auto it = from; it != to; ++it)
        {
            if (wanted(it->second))
            {
                return it->second;
            }
        }
    }
    return std::nullopt;
}

// --------------------------------------------------------------- one mesh

// A glTF mesh's triangles, gathered from its primitives, in glTF's space.
struct Gathered
{
    std::vector<Vec3> places;
    std::vector<Vec3> normals;
    std::vector<Value> tangents;
    std::vector<std::vector<Value>> uvs;
    std::vector<std::vector<Value>> colours;
    std::vector<Value> joints;
    std::vector<Value> weights;
    std::vector<std::uint32_t> indices;
    std::string material;
};

void Append(std::vector<Value> &to, const std::vector<Value> &from, std::size_t count, bool wanted)
{
    if (wanted)
    {
        to.insert(to.end(), from.begin(), from.end());
        to.resize(to.size() - from.size() + count, Value{0, 0, 0, 1});
    }
}

class Gatherer
{
  public:
    Gatherer(const GltfFile &gltf, std::vector<std::string> &notes, const std::string &label)
        : gltf_(gltf), notes_(notes), label_(label)
    {
    }

    Gathered Gather(const Json &mesh)
    {
        Gathered out;
        const Json &primitives = mesh.value("primitives", Json::array());
        // A set is kept only if every primitive has it, so every vertex has one.
        has_ = {true, true, true, true};
        sets_ = 8;
        colourSets_ = 8;
        for (const auto &p : primitives)
        {
            const Json &a = p.value("attributes", Json::object());
            has_[0] = has_[0] && a.contains("NORMAL");
            has_[1] = has_[1] && a.contains("TANGENT");
            has_[2] = has_[2] && a.contains("JOINTS_0") && a.contains("WEIGHTS_0");
            sets_ = std::min(sets_, Count(a, "TEXCOORD_"));
            colourSets_ = std::min(colourSets_, Count(a, "COLOR_"));
            if (a.contains("JOINTS_1"))
            {
                Note("its vertices have more than four bones each (JOINTS_1); the first four are kept. Export "
                     "with influences limited to four");
            }
        }
        out.uvs.resize(sets_);
        out.colours.resize(colourSets_);
        for (const auto &p : primitives)
        {
            Primitive(p, out);
        }
        return out;
    }

    [[nodiscard]] bool HasNormals() const
    {
        return has_[0];
    }
    [[nodiscard]] bool HasTangents() const
    {
        return has_[1];
    }
    [[nodiscard]] bool Skinned() const
    {
        return has_[2];
    }

  private:
    static std::size_t Count(const Json &attributes, const std::string &prefix)
    {
        std::size_t n = 0;
        while (attributes.contains(prefix + std::to_string(n)))
        {
            ++n;
        }
        return n;
    }

    void Primitive(const Json &p, Gathered &out)
    {
        if (p.value("mode", 4) != 4)
        {
            Note("a primitive that is not a triangle list is left out");
            return;
        }
        const Json &a = p["attributes"];
        if (!a.contains("POSITION"))
        {
            Note("a primitive with no positions is left out");
            return;
        }
        const auto base = static_cast<std::uint32_t>(out.places.size());
        const auto places = Values(gltf_, a["POSITION"].get<std::size_t>());
        const std::size_t n = places.size();
        for (const auto &v : places)
        {
            out.places.push_back({v[0], v[1], v[2]});
        }
        if (has_[0])
        {
            for (const auto &v : Values(gltf_, a["NORMAL"].get<std::size_t>()))
            {
                out.normals.push_back({v[0], v[1], v[2]});
            }
            out.normals.resize(out.places.size(), Vec3{0, 0, 1});
        }
        Append(out.tangents, has_[1] ? Values(gltf_, a["TANGENT"].get<std::size_t>()) : std::vector<Value>{}, n,
               has_[1]);
        for (std::size_t s = 0; s < sets_; ++s)
        {
            Append(out.uvs[s], Values(gltf_, a["TEXCOORD_" + std::to_string(s)].get<std::size_t>()), n, true);
        }
        for (std::size_t s = 0; s < colourSets_; ++s)
        {
            Append(out.colours[s], Values(gltf_, a["COLOR_" + std::to_string(s)].get<std::size_t>()), n, true);
        }
        if (has_[2])
        {
            Append(out.joints, Values(gltf_, a["JOINTS_0"].get<std::size_t>()), n, true);
            Append(out.weights, Values(gltf_, a["WEIGHTS_0"].get<std::size_t>()), n, true);
        }
        if (p.contains("indices"))
        {
            for (const auto &i : Values(gltf_, p["indices"].get<std::size_t>()))
            {
                const auto index = static_cast<std::uint32_t>(i[0]);
                if (index >= n)
                {
                    throw std::runtime_error(label_ + ": an index past its primitive's vertices");
                }
                out.indices.push_back(base + index);
            }
        }
        else
        {
            for (std::uint32_t i = 0; i < n; ++i)
            {
                out.indices.push_back(base + i);
            }
        }
        if (out.material.empty() && p.contains("material"))
        {
            const Json &materials = gltf_.document.value("materials", Json::array());
            const auto m = p["material"].get<std::size_t>();
            if (m < materials.size())
            {
                out.material = materials[m].value("name", std::string());
            }
        }
    }

    void Note(const std::string &text)
    {
        notes_.push_back(label_ + ": " + text);
    }

    const GltfFile &gltf_;
    std::vector<std::string> &notes_;
    const std::string &label_;
    std::array<bool, 4> has_{};
    std::size_t sets_ = 0;
    std::size_t colourSets_ = 0;
};

// Normals from the triangles, each vertex's the sum of its faces', where the glTF has none.
std::vector<Vec3> FaceNormals(const std::vector<Vec3> &p, const std::vector<std::uint32_t> &indices)
{
    std::vector<Vec3> sum(p.size(), Vec3{0, 0, 0});
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const Vec3 &a = p[indices[i]];
        const Vec3 &b = p[indices[i + 1]];
        const Vec3 &c = p[indices[i + 2]];
        const Vec3 face = Cross({b[0] - a[0], b[1] - a[1], b[2] - a[2]}, {c[0] - a[0], c[1] - a[1], c[2] - a[2]});
        for (std::size_t k = 0; k < 3; ++k)
        {
            for (std::size_t c3 = 0; c3 < 3; ++c3)
            {
                sum[indices[i + k]][c3] += face[c3];
            }
        }
    }
    for (auto &n : sum)
    {
        n = Normalised(n, {0, 0, 1});
    }
    return sum;
}

// Tangents from the texture coordinates, each vertex's the sum of its
// faces' made square to its normal, its w the side the faces' bitangent is
// on, where the glTF has none (Blender exports none unless asked).
std::vector<Value> UvTangents(const std::vector<Vec3> &p, const std::vector<Vec3> &n, const std::vector<Value> &uv,
                              const std::vector<std::uint32_t> &indices)
{
    std::vector<Vec3> t(p.size(), Vec3{0, 0, 0});
    std::vector<Vec3> b(p.size(), Vec3{0, 0, 0});
    for (std::size_t i = 0; i + 2 < indices.size() && !uv.empty(); i += 3)
    {
        const std::uint32_t i0 = indices[i];
        const std::uint32_t i1 = indices[i + 1];
        const std::uint32_t i2 = indices[i + 2];
        const Vec3 e1{p[i1][0] - p[i0][0], p[i1][1] - p[i0][1], p[i1][2] - p[i0][2]};
        const Vec3 e2{p[i2][0] - p[i0][0], p[i2][1] - p[i0][1], p[i2][2] - p[i0][2]};
        const float du1 = uv[i1][0] - uv[i0][0];
        const float dv1 = uv[i1][1] - uv[i0][1];
        const float du2 = uv[i2][0] - uv[i0][0];
        const float dv2 = uv[i2][1] - uv[i0][1];
        const float det = (du1 * dv2) - (du2 * dv1);
        if (std::abs(det) < 1e-12F)
        {
            continue;
        }
        const float r = 1.0F / det;
        for (const std::uint32_t v : {i0, i1, i2})
        {
            for (std::size_t c = 0; c < 3; ++c)
            {
                t[v][c] += ((e1[c] * dv2) - (e2[c] * dv1)) * r;
                b[v][c] += ((e2[c] * du1) - (e1[c] * du2)) * r;
            }
        }
    }
    std::vector<Value> out(p.size());
    for (std::size_t v = 0; v < p.size(); ++v)
    {
        const float d = Dot(n[v], t[v]);
        Vec3 square{t[v][0] - (n[v][0] * d), t[v][1] - (n[v][1] * d), t[v][2] - (n[v][2] * d)};
        // A vertex no textured face gives a direction: any one square to its normal.
        const Vec3 any = std::abs(n[v][0]) < 0.9F ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        square = Normalised(square, Normalised(Cross(any, n[v]), {1, 0, 0}));
        const float w = Dot(Cross(n[v], square), b[v]) < 0 ? -1.0F : 1.0F;
        out[v] = {square[0], square[1], square[2], w};
    }
    return out;
}

// One glTF mesh node made a chunk's data.
class ChunkImport
{
  public:
    ChunkImport(const GltfFile &gltf, const model::Hierarchy &tree, const std::vector<Mat4> &gltfWorlds,
                const std::multimap<std::string, int> &byName, Imported &out)
        : gltf_(gltf), tree_(tree), gltfWorlds_(gltfWorlds), byName_(byName), out_(out)
    {
    }

    // The chunk's data, with `daoNode` the model's mesh node it is drawn by.
    model::ChunkData Make(std::size_t gltfNode, const model::Chunk &chunk, std::size_t chunkIndex, int daoNode,
                          const std::string &label)
    {
        const Json &node = gltf_.document["nodes"][gltfNode];
        const Json &mesh = gltf_.document["meshes"][node["mesh"].get<std::size_t>()];
        const model::MeshRef &ref = *tree_.nodes[static_cast<std::size_t>(daoNode)].mesh;
        Gatherer gatherer(gltf_, out_.notes, label);
        Gathered g = gatherer.Gather(mesh);
        if (g.places.empty())
        {
            throw std::runtime_error(label + ": no triangles");
        }
        const bool wantsBones = chunk.Find(model::Usage::BlendIndices) != nullptr;
        const bool skinned = gatherer.Skinned() && node.contains("skin");
        if (wantsBones && !skinned)
        {
            throw std::runtime_error(label + ": the chunk is skinned in the game, and the glTF's mesh has no "
                                             "joints and weights; export it with its armature");
        }
        Placed placed = Place(gltfNode, g, skinned, daoNode);
        std::vector<Vec3> normals = gatherer.HasNormals() ? placed.normals : FaceNormals(placed.places, g.indices);
        if (!gatherer.HasNormals())
        {
            out_.notes.push_back(label + ": no normals in the glTF; made from the faces");
        }
        std::vector<Value> tangents =
            gatherer.HasTangents()
                ? placed.tangents
                : UvTangents(placed.places, normals, g.uvs.empty() ? std::vector<Value>{} : g.uvs[0], g.indices);
        std::vector<std::int32_t> bonesUsed = ref.bonesUsed;
        std::vector<Value> slots;
        std::vector<Value> weights;
        if (wantsBones)
        {
            Bones(node, g, bonesUsed, slots, weights, label);
        }
        model::ChunkData data;
        data.name = chunk.name;
        data.index = chunkIndex;
        data.indices = g.indices;
        if (Turned(mesh))
        {
            for (std::size_t i = 0; i + 2 < data.indices.size(); i += 3)
            {
                std::swap(data.indices[i + 1], data.indices[i + 2]);
            }
        }
        const Streams streams{placed.places, normals, tangents, g.uvs, g.colours, slots, weights};
        for (const model::Element &element : chunk.declaration)
        {
            data.values.push_back(Stream(element, streams, label));
        }
        Edits(daoNode, ref, bonesUsed, g.material);
        out_.changes.push_back(Json{{"chunk", Utf8(chunk.name)},
                                    {"vertices", {{"before", chunk.vertexCount}, {"after", placed.places.size()}}},
                                    {"triangles", {{"before", chunk.indexCount / 3}, {"after", g.indices.size() / 3}}},
                                    {"bones_used", {{"before", ref.bonesUsed.size()}, {"after", bonesUsed.size()}}}});
        if (placed.places.size() != chunk.vertexCount)
        {
            out_.notes.push_back(label + ": its vertex count changed (" + std::to_string(chunk.vertexCount) + " to " +
                                 std::to_string(placed.places.size()) +
                                 "); a head's morphs (.mor) replace every vertex by its place, so a head's no longer "
                                 "fit");
        }
        return data;
    }

  private:
    struct Placed
    {
        std::vector<Vec3> places;
        std::vector<Vec3> normals;
        std::vector<Value> tangents;
    };

    struct Streams
    {
        const std::vector<Vec3> &places;
        const std::vector<Vec3> &normals;
        const std::vector<Value> &tangents;
        const std::vector<std::vector<Value>> &uvs;
        const std::vector<std::vector<Value>> &colours;
        const std::vector<Value> &slots;
        const std::vector<Value> &weights;
    };

    static bool Turned(const Json &mesh)
    {
        const Json dao = mesh.value("extras", Json::object()).value("dao", Json::object());
        return dao.value("winding_turned", false);
    }

    // Each joint's world matrix times its inverse bind matrix: what moves a
    // vertex at rest from its mesh's space to the world, for that joint.
    std::vector<Mat4> JointMatrices(const Json &node) const
    {
        const Json &skin = gltf_.document["skins"].at(node["skin"].get<std::size_t>());
        const Json &joints = skin["joints"];
        const std::vector<Mat4> inverse = skin.contains("inverseBindMatrices")
                                              ? Matrices(gltf_, skin["inverseBindMatrices"].get<std::size_t>())
                                              : std::vector<Mat4>(joints.size(), Transform({0, 0, 0}, {0, 0, 0, 1}, 1));
        std::vector<Mat4> out;
        out.reserve(joints.size());
        for (std::size_t j = 0; j < joints.size(); ++j)
        {
            out.push_back(Times(gltfWorlds_.at(joints[j].get<std::size_t>()), inverse.at(j)));
        }
        return out;
    }

    // A skinned vertex's matrix: its joints', weighted; its node's with no weight.
    static Mat4 Skinning(const Value &joints, const Value &weights, const std::vector<Mat4> &jointMatrices,
                         const Mat4 &own)
    {
        Mat4 m{};
        float sum = 0;
        for (std::size_t k = 0; k < 4; ++k)
        {
            const auto j = static_cast<std::size_t>(joints[k]);
            if (weights[k] <= 0 || j >= jointMatrices.size())
            {
                continue;
            }
            sum += weights[k];
            for (std::size_t c = 0; c < 16; ++c)
            {
                m[c] += weights[k] * jointMatrices[j][c];
            }
        }
        if (sum <= 0)
        {
            return own;
        }
        for (double &c : m)
        {
            c /= sum;
        }
        return m;
    }

    // Each vertex where the glTF has it at rest, in the space of the model's
    // mesh node and the game's axes. A skinned vertex is placed by its joints
    // (their world matrices times their inverse bind matrices, weighted), as
    // a viewer draws it at rest; another by its node's world matrix.
    Placed Place(std::size_t gltfNode, const Gathered &g, bool skinned, int daoNode)
    {
        const Json &node = gltf_.document["nodes"][gltfNode];
        const Mat4 back = Inverse(Worlds(tree_)[static_cast<std::size_t>(daoNode)]);
        const std::vector<Mat4> jointMatrices = skinned ? JointMatrices(node) : std::vector<Mat4>{};
        const Mat4 &own = gltfWorlds_.at(gltfNode);
        Placed out;
        for (std::size_t v = 0; v < g.places.size(); ++v)
        {
            const Mat4 toDao = Times(back, skinned ? Skinning(g.joints[v], g.weights[v], jointMatrices, own) : own);
            out.places.push_back(Back(Apply(toDao, g.places[v])));
            if (!g.normals.empty())
            {
                out.normals.push_back(Back(Normalised(Turn(toDao, g.normals[v]), {0, 1, 0})));
            }
            if (!g.tangents.empty())
            {
                const Vec3 t =
                    Back(Normalised(Turn(toDao, {g.tangents[v][0], g.tangents[v][1], g.tangents[v][2]}), {1, 0, 0}));
                out.tangents.push_back({t[0], t[1], t[2], g.tangents[v][3] < 0 ? -1.0F : 1.0F});
            }
        }
        return out;
    }

    // The model's bone of each joint of a mesh node's skin, by name; -1 for
    // none, with the joint's name.
    struct JointBones
    {
        std::vector<std::int32_t> bones;
        std::vector<std::string> names;
    };

    JointBones BonesOfJoints(const Json &node) const
    {
        const Json &joints = gltf_.document["skins"].at(node["skin"].get<std::size_t>())["joints"];
        JointBones out;
        for (const auto &j : joints)
        {
            const std::string name = gltf_.document["nodes"].at(j.get<std::size_t>()).value("name", std::string());
            const auto found = NodeNamed(
                byName_, name, [&](int i) { return tree_.nodes[static_cast<std::size_t>(i)].bone.has_value(); });
            out.bones.push_back(found ? tree_.nodes[static_cast<std::size_t>(*found)].bone.value_or(-1) : -1);
            out.names.push_back(name);
        }
        return out;
    }

    // A joint's slot in the mesh node's bones, the bone added to them if they
    // do not list it yet.
    static float Slot(const JointBones &boneOf, std::size_t joint, std::vector<std::int32_t> &bonesUsed,
                      const std::string &label)
    {
        const std::int32_t bone = joint < boneOf.bones.size() ? boneOf.bones[joint] : -1;
        if (bone < 0)
        {
            std::string message = label;
            message += ": a vertex is weighted to \"";
            message += joint < boneOf.names.size() ? boneOf.names[joint] : std::to_string(joint);
            message += "\", which is no bone of the model; bones cannot be added";
            throw std::runtime_error(message);
        }
        auto at = std::ranges::find(bonesUsed, bone);
        if (at == bonesUsed.end())
        {
            bonesUsed.push_back(bone);
            at = bonesUsed.end() - 1;
        }
        return static_cast<float>(at - bonesUsed.begin());
    }

    // Each vertex's blend indices, slots in the mesh node's bones, and their
    // weights: a glTF joint is the model's bone of the same name; a bone the
    // mesh node does not list yet is added to its list.
    void Bones(const Json &node, const Gathered &g, std::vector<std::int32_t> &bonesUsed, std::vector<Value> &slots,
               std::vector<Value> &weights, const std::string &label)
    {
        const JointBones boneOf = BonesOfJoints(node);
        for (std::size_t v = 0; v < g.joints.size(); ++v)
        {
            Value slot{0, 0, 0, 0};
            Value weight{0, 0, 0, 0};
            float sum = 0;
            for (std::size_t k = 0; k < 4; ++k)
            {
                const float w = std::max(g.weights[v][k], 0.0F);
                if (w > 0)
                {
                    slot[k] = Slot(boneOf, static_cast<std::size_t>(g.joints[v][k]), bonesUsed, label);
                    weight[k] = w;
                    sum += w;
                }
            }
            for (float &w : weight)
            {
                w = sum > 0 ? w / sum : 0;
            }
            if (sum <= 0)
            {
                weight[0] = 1;
            }
            slots.push_back(slot);
            weights.push_back(weight);
        }
        if (bonesUsed.size() > model::kMostBonesInChunk)
        {
            // The skinning shaders' palette, float4 bsm_bones[201], three
            // rows a bone (lib-agent, 2026-10-14, DAOLib #117).
            throw std::runtime_error(label + ": its vertices are weighted to " + std::to_string(bonesUsed.size()) +
                                     " bones, and the game's skinning shaders take at most " +
                                     std::to_string(model::kMostBonesInChunk) +
                                     " for one chunk; weight it to fewer bones");
        }
    }

    // One declaration element's values for every vertex.
    std::vector<Value> Stream(const model::Element &element, const Streams &s, const std::string &label)
    {
        const std::size_t n = s.places.size();
        std::vector<Value> out(n, Value{0, 0, 0, 1});
        const auto three = [&](const std::vector<Vec3> &from) {
            for (std::size_t v = 0; v < n; ++v)
            {
                out[v] = {from[v][0], from[v][1], from[v][2], 1};
            }
        };
        const auto set = [&](const std::vector<std::vector<Value>> &sets, const char *what, Value missing) {
            if (element.usageIndex < sets.size())
            {
                out = sets[element.usageIndex];
                return;
            }
            std::ranges::fill(out, missing);
            out_.notes.push_back(label + ": no " + what + " set " + std::to_string(element.usageIndex) +
                                 " in the glTF; written as " + (missing[0] == 0 ? "0" : "1"));
        };
        switch (element.usage)
        {
        case model::Usage::Position:
            three(s.places);
            break;
        case model::Usage::Normal:
            three(s.normals);
            break;
        case model::Usage::Tangent:
            for (std::size_t v = 0; v < n; ++v)
            {
                out[v] = {s.tangents[v][0], s.tangents[v][1], s.tangents[v][2], 1};
            }
            break;
        case model::Usage::Binormal:
            // The game keeps the binormal and no sign: w * cross(N, T), the
            // export's reading turned back.
            for (std::size_t v = 0; v < n; ++v)
            {
                const Vec3 t{s.tangents[v][0], s.tangents[v][1], s.tangents[v][2]};
                const Vec3 b = Cross(s.normals[v], t);
                const float w = s.tangents[v][3];
                out[v] = {b[0] * w, b[1] * w, b[2] * w, 1};
            }
            break;
        case model::Usage::TexCoord:
            set(s.uvs, "texture coordinate", Value{0, 0, 0, 1});
            break;
        case model::Usage::Color:
            set(s.colours, "colour", Value{1, 1, 1, 1});
            break;
        case model::Usage::BlendIndices:
            out = s.slots;
            break;
        case model::Usage::BlendWeight:
            out = s.weights;
            break;
        default:
            out_.notes.push_back(label + ": its " + UsageName(element.usage) +
                                 " has no glTF counterpart; written as 0");
            std::ranges::fill(out, Value{0, 0, 0, 0});
            break;
        }
        return out;
    }

    void Edits(int daoNode, const model::MeshRef &ref, const std::vector<std::int32_t> &bonesUsed,
               const std::string &material)
    {
        model::NodeEdit edit;
        edit.node = daoNode;
        bool changed = false;
        if (bonesUsed != ref.bonesUsed)
        {
            edit.bonesUsed = bonesUsed;
            changed = true;
        }
        const std::string stem = Unnumbered(material);
        if (!stem.empty() && Lower(stem) != Lower(Utf8(ref.material)))
        {
            edit.material = Wide(stem);
            changed = true;
        }
        if (changed)
        {
            out_.edits.push_back(std::move(edit));
        }
    }

    const GltfFile &gltf_;
    const model::Hierarchy &tree_;
    const std::vector<Mat4> &gltfWorlds_;
    const std::multimap<std::string, int> &byName_;
    Imported &out_;
};

// The chunk a glTF mesh node is: by the export's extras, else the mesh's
// name, else its node's.
std::optional<std::size_t> ChunkOf(const Json &node, const Json &mesh, const model::Hierarchy &tree,
                                   const model::Mesh &daoMesh, const std::multimap<std::string, int> &byName)
{
    const Json dao = mesh.value("extras", Json::object()).value("dao", Json::object());
    const auto named = [&](const std::string &name) -> std::optional<std::size_t> {
        std::optional<std::size_t> found;
        for (std::size_t i = 0; i < daoMesh.chunks.size(); ++i)
        {
            const std::string chunk = Lower(Utf8(daoMesh.chunks[i].name));
            if (chunk == Lower(name) || chunk == Lower(Unnumbered(name)))
            {
                if (found)
                {
                    return std::nullopt; // a repeated name, which only the index tells apart
                }
                found = i;
            }
        }
        return found;
    };
    if (dao.contains("chunk_index"))
    {
        const auto i = dao["chunk_index"].get<std::size_t>();
        if (i < daoMesh.chunks.size() &&
            Lower(Utf8(daoMesh.chunks[i].name)) == Lower(dao.value("chunk", Utf8(daoMesh.chunks[i].name))))
        {
            return i;
        }
    }
    for (const std::string &name : {dao.value("chunk", std::string()), mesh.value("name", std::string())})
    {
        if (!name.empty())
        {
            if (const auto i = named(name))
            {
                return i;
            }
        }
    }
    const auto daoNode = NodeNamed(byName, node.value("name", std::string()),
                                   [&](int i) { return tree.nodes[static_cast<std::size_t>(i)].mesh.has_value(); });
    if (daoNode)
    {
        if (const auto &ref = tree.nodes[static_cast<std::size_t>(*daoNode)].mesh)
        {
            return named(Utf8(ref->chunk));
        }
    }
    return std::nullopt;
}

// The model's mesh node that draws a chunk.
std::optional<int> MeshNodeOf(const model::Hierarchy &tree, const model::Chunk &chunk)
{
    for (std::size_t i = 0; i < tree.nodes.size(); ++i)
    {
        const auto &mesh = tree.nodes[i].mesh;
        if (mesh && Lower(Utf8(mesh->chunk)) == Lower(Utf8(chunk.name)))
        {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

} // namespace

GltfFile ReadGltf(const std::filesystem::path &path)
{
    GltfFile out;
    const std::vector<std::byte> bytes = ReadBytes(path);
    std::optional<std::vector<std::byte>> bin;
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "glTF", 4) == 0)
    {
        // A .glb: its JSON chunk, then its BIN chunk, each a length and a type.
        std::size_t at = 12;
        while (at + 8 <= bytes.size())
        {
            const std::uint32_t length = U32(bytes, at);
            const std::uint32_t type = U32(bytes, at + 4);
            if (at + 8 + length > bytes.size())
            {
                throw std::runtime_error(PathText(path) + ": a chunk runs past the file's end");
            }
            const auto *data = reinterpret_cast<const char *>(bytes.data() + at + 8);
            if (type == 0x4E4F534A)
            {
                out.document = Json::parse(std::string_view(data, length));
            }
            else if (type == 0x004E4942)
            {
                bin.emplace(bytes.begin() + static_cast<std::ptrdiff_t>(at + 8),
                            bytes.begin() + static_cast<std::ptrdiff_t>(at + 8 + length));
            }
            at += 8 + length;
        }
    }
    else
    {
        out.document = Json::parse(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
    }
    if (!out.document.is_object() || out.document.value("asset", Json::object()).value("version", "") != "2.0")
    {
        throw std::runtime_error(PathText(path) + " is no glTF 2.0 file");
    }
    for (const auto &buffer : out.document.value("buffers", Json::array()))
    {
        if (!buffer.contains("uri"))
        {
            if (!bin)
            {
                throw std::runtime_error(PathText(path) + ": a buffer with no uri, and no BIN chunk");
            }
            out.buffers.push_back(*bin);
            continue;
        }
        const std::string uri = buffer["uri"].get<std::string>();
        if (uri.starts_with("data:"))
        {
            throw std::runtime_error(PathText(path) + ": a buffer embedded in the JSON is not read; export as .glb, "
                                                      "or as .gltf with a separate .bin");
        }
        out.buffers.push_back(ReadBytes(path.parent_path() / std::filesystem::path(Wide(Unescaped(uri)))));
    }
    return out;
}

Imported ModelFromGltf(const GltfFile &gltf, const model::Hierarchy &tree, const model::Mesh &mesh)
{
    Imported out;
    const Json &nodes = gltf.document.value("nodes", Json::array());
    const Json &meshes = gltf.document.value("meshes", Json::array());
    const std::vector<Mat4> worlds = SceneWorlds(nodes);
    const auto byName = NodesByName(tree);
    ChunkImport import(gltf, tree, worlds, byName, out);
    std::vector<bool> given(mesh.chunks.size(), false);
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const Json &node = nodes[i];
        if (!node.contains("mesh"))
        {
            continue;
        }
        const Json &gm = meshes.at(node["mesh"].get<std::size_t>());
        const std::string label =
            "mesh " + gm.value("name", std::string("?")) + " (node " + node.value("name", std::string("?")) + ")";
        const auto chunk = ChunkOf(node, gm, tree, mesh, byName);
        if (!chunk)
        {
            out.notes.push_back(label + ": is none of the model's chunks; left out");
            continue;
        }
        if (given[*chunk])
        {
            out.notes.push_back(label + ": its chunk is given by another mesh already; left out");
            continue;
        }
        const auto daoNode = MeshNodeOf(tree, mesh.chunks[*chunk]);
        if (!daoNode)
        {
            out.notes.push_back(label + ": its chunk is drawn by no node of the model; left out");
            continue;
        }
        out.chunks.push_back(import.Make(i, mesh.chunks[*chunk], *chunk, *daoNode, label));
        given[*chunk] = true;
    }
    if (out.chunks.empty())
    {
        throw std::runtime_error("none of the glTF's meshes is one of the model's chunks (by the extras `model_export` "
                                 "wrote, or by name)");
    }
    for (std::size_t c = 0; c < mesh.chunks.size(); ++c)
    {
        if (!given[c])
        {
            out.notes.push_back("chunk " + Utf8(mesh.chunks[c].name) + " is in no mesh of the glTF; kept as it was");
        }
    }
    return out;
}

} // namespace eluvian::ghilana
