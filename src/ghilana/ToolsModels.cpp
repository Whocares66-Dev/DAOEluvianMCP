// Ghilana's tools over the game's models (#132): what a model is made of
// (model), and a model written out as glTF for Blender (model_export), the
// first half of a round trip whose way back is DAOLib #117's writer. Both
// read a model by its hierarchy (.mmh), which names its mesh file (.msh);
// DAOLib's core reads both (dao/core/Model.h, DAOLib #116), Gltf.h puts
// them in glTF's terms.
#include "ghilana/Ghilana.h"

#include "core/Args.h"
#include "ghilana/Gltf.h"
#include "ghilana/GltfImport.h"
#include "ghilana/Text.h"

#include <dao/core/Gff4.h>
#include <dao/core/Model.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <memory>

namespace eluvian::ghilana::detail
{

namespace
{

namespace gff4 = dao::core::gff4;
namespace model = dao::core::model;

ToolSpec Spec(std::string name, std::string description, Json properties, const std::vector<std::string> &required,
              bool readOnly = true)
{
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    schema["required"] = required;
    return ToolSpec{std::move(name), std::move(description), std::move(schema), Thread::Listener, readOnly};
}

const Json kModelProperties = {
    {"name",
     {{"type", "string"},
      {"description", "The model's hierarchy, \"cn_bdy_ogra_0.mmh\" (the extension may be left off). `resources` "
                      "with type mmh finds one; it names its mesh file (.msh) itself."}}},
    {"source",
     {{"type", "string"},
      {"description", "Which copy of the .mmh, by its source (\"core\", \"addin:...\"). Default: the copy the game "
                      "takes. Its mesh file is always the game's copy."}}},
    {"archive", {{"type", "string"}, {"description", "Which copy of the .mmh, by its archive's path."}}},
};

// A model's two files, read, and what is read of them; the files' bytes
// stay here, since the mesh's chunks point into them.
struct Loaded
{
    std::string treeName;
    std::string meshName;
    std::vector<std::byte> treeBytes;
    std::vector<std::byte> meshBytes;
    model::Hierarchy tree;
    model::Mesh mesh;
    Json treeFrom;
    Json meshFrom;
};

std::vector<std::byte> ReadCopy(const GameFiles &files, const Copy &copy, const std::string &tool,
                                const std::string &name)
{
    std::string why;
    auto bytes = files.Read(copy, &why);
    if (!bytes)
    {
        throw ToolError(tool + ": \"" + name + "\" cannot be read: " + why);
    }
    return std::move(*bytes);
}

std::unique_ptr<Loaded> Load(Ghilana &ghilana, const Args &args, const std::string &tool)
{
    auto out = std::make_unique<Loaded>();
    std::string name = Lower(args.String("name"));
    if (Extension(name).empty())
    {
        name += ".mmh";
    }
    if (Extension(name) == "msh")
    {
        throw ToolError(tool + ": name the model's hierarchy (.mmh), which names its mesh (.msh); `resources` with "
                               "type mmh and a word of the name finds it");
    }
    (void)ghilana.Indexed();
    const GameFiles &files = ghilana.Files();
    const auto copies = files.Select(tool, name, args.String("source", ""), args.String("archive", ""));
    const Copy &copy = GameFiles::Choose(copies);
    out->treeName = name;
    out->treeBytes = ReadCopy(files, copy, tool, name);
    const auto treeFile = gff4::File::Parse(out->treeBytes);
    auto tree = treeFile ? model::HierarchyOf(*treeFile) : std::nullopt;
    if (!tree)
    {
        throw ToolError(tool + ": \"" + name + "\" is no model hierarchy DAOLib reads; `resource` shows what it holds");
    }
    out->tree = std::move(*tree);
    out->treeFrom = Chosen(files, name, *files.Find(name), copy);

    std::string meshName = Lower(Utf8(out->tree.meshFile));
    if (Extension(meshName).empty())
    {
        meshName += ".msh";
    }
    out->meshName = meshName;
    const auto *meshCopies = files.Find(meshName);
    if (meshCopies == nullptr)
    {
        throw ToolError(tool + ": \"" + name + "\" names the mesh file \"" + meshName +
                        "\", which the game has no copy of");
    }
    const Copy &meshCopy = GameFiles::Choose(*meshCopies);
    out->meshBytes = ReadCopy(files, meshCopy, tool, meshName);
    const auto meshFile = gff4::File::Parse(out->meshBytes);
    auto mesh = meshFile ? model::MeshOf(*meshFile) : std::nullopt;
    if (!mesh)
    {
        throw ToolError(tool + ": \"" + meshName + "\" is no mesh file DAOLib reads");
    }
    out->mesh = std::move(*mesh);
    out->meshFrom = Chosen(files, meshName, *meshCopies, meshCopy);
    return out;
}

const model::Chunk *ChunkNamed(const model::Mesh &mesh, const std::wstring &name)
{
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

std::string ElementText(const model::Element &e)
{
    return UsageName(e.usage) + " " + std::to_string(e.usageIndex) + " " + FormatName(e.format);
}

Json Bounds(const model::Chunk &chunk)
{
    const model::Element *position = chunk.Find(model::Usage::Position);
    if (position == nullptr)
    {
        return nullptr;
    }
    const auto places = model::Floats(chunk, *position);
    if (places.empty())
    {
        return nullptr;
    }
    std::array<float, 3> low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
    std::array<float, 3> high{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                              std::numeric_limits<float>::lowest()};
    for (const auto &p : places)
    {
        for (std::size_t c = 0; c < 3; ++c)
        {
            low[c] = std::min(low[c], p[c]);
            high[c] = std::max(high[c], p[c]);
        }
    }
    return Json{{"min", low}, {"max", high}};
}

Json MeshJson(const model::Node &node, const model::Mesh &mesh)
{
    const model::MeshRef &ref = *node.mesh;
    Json out = {{"node", Utf8(node.name)}, {"chunk", Utf8(ref.chunk)}, {"material", Utf8(ref.material)}};
    if (!ref.part.empty())
    {
        out["part"] = Utf8(ref.part);
    }
    const model::Chunk *chunk = ChunkNamed(mesh, ref.chunk);
    if (chunk == nullptr)
    {
        out["missing"] = "the mesh file has no chunk of this name";
        return out;
    }
    out["vertices"] = chunk->vertexCount;
    out["triangles"] = chunk->indexCount / 3;
    Json elements = Json::array();
    for (const auto &e : chunk->declaration)
    {
        elements.push_back(ElementText(e));
    }
    out["elements"] = std::move(elements);
    const bool skinned = chunk->Find(model::Usage::BlendIndices) != nullptr && !ref.bonesUsed.empty();
    out["skinned"] = skinned;
    if (skinned)
    {
        out["bones_used"] = ref.bonesUsed.size();
    }
    if (Json bounds = Bounds(*chunk); !bounds.is_null())
    {
        out["bounds"] = std::move(bounds);
    }
    return out;
}

// ---------------------------------------------------------------- model

Json Model(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "model");
    const auto loaded = Load(ghilana, args, "model");
    const model::Hierarchy &tree = loaded->tree;
    Json out = {{"model", loaded->treeFrom}, {"mesh_file", loaded->meshFrom}};
    std::map<std::string, int> kinds;
    Json meshes = Json::array();
    for (const auto &node : tree.nodes)
    {
        ++kinds[node.kind];
        if (node.mesh)
        {
            meshes.push_back(MeshJson(node, loaded->mesh));
        }
    }
    out["nodes"] = tree.nodes.size();
    out["kinds"] = kinds;
    const auto carried = std::ranges::count_if(tree.bones, [](int node) { return node >= 0; });
    out["bones"] = Json{{"total", tree.totalBones}, {"with_a_node", carried}};
    out["meshes"] = std::move(meshes);
    out["chunks_in_mesh_file"] = loaded->mesh.chunks.size();
    out["units"] = "the file's: Z up; bounds are each chunk's vertices as the file keeps them";
    if (args.Bool("tree", false))
    {
        Json list = Json::array();
        for (const auto &node : tree.nodes)
        {
            Json item = {{"name", Utf8(node.name)}, {"kind", node.kind}, {"parent", node.parent}};
            if (node.bone)
            {
                item["bone"] = *node.bone;
            }
            list.push_back(std::move(item));
        }
        out["tree"] = std::move(list);
    }
    out["next"] = "`model_export` writes it as glTF for Blender";
    return out;
}

// ---------------------------------------------------------------- model_export

void WriteFile(const fs::path &path, std::span<const std::byte> bytes, const std::string &tool)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
    {
        throw ToolError(tool + ": could not write " + PathText(path));
    }
}

// The folder a call's `to` names, made: a full path, not in the game's folder.
fs::path OutputFolder(Ghilana &ghilana, const Args &args, const std::string &tool)
{
    const fs::path to = fs::path(Wide(args.String("to")));
    if (!to.is_absolute())
    {
        throw ToolError(tool + R"(: `to` is a folder's full path ("C:\\work\\models"))");
    }
    if (Inside(to, ghilana.Files().Where().game))
    {
        throw ToolError(tool + ": `to` is inside the game's folder, which ghilana never writes into; name a folder "
                               "of your own");
    }
    std::error_code error;
    fs::create_directories(to, error);
    if (error)
    {
        throw ToolError(tool + ": cannot make " + PathText(to) + ": " + error.message());
    }
    return to;
}

Json Export(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "model_export");
    const fs::path to = OutputFolder(ghilana, args, "model_export");
    const auto loaded = Load(ghilana, args, "model_export");
    Gltf gltf = ModelGltf(loaded->tree, loaded->mesh);
    const std::string stem = Stem(loaded->treeName);
    const fs::path gltfPath = to / fs::path(Wide(stem + ".gltf"));
    const fs::path binPath = to / fs::path(Wide(stem + ".bin"));
    if (gltf.document.contains("buffers"))
    {
        gltf.document["buffers"][0]["uri"] = stem + ".bin";
        WriteFile(binPath, gltf.binary, "model_export");
    }
    const std::string text = gltf.document.dump(1);
    WriteFile(gltfPath, std::as_bytes(std::span(text)), "model_export");

    const Json &doc = gltf.document;
    Json out = {{"model", loaded->treeFrom}, {"mesh_file", loaded->meshFrom}, {"gltf", PathText(gltfPath)}};
    if (doc.contains("buffers"))
    {
        out["bin"] = PathText(binPath);
    }
    out["nodes"] = doc["nodes"].size();
    out["meshes"] = doc.contains("meshes") ? doc["meshes"].size() : 0;
    out["materials"] = doc.contains("materials") ? doc["materials"].size() : 0;
    out["joints"] = doc.contains("skins") ? doc["skins"][0]["joints"].size() : 0;
    if (!gltf.notes.empty())
    {
        out["notes"] = gltf.notes;
    }
    out["textures"] = "not written yet: each material is named for the .mao it is (`resource` reads one as text), "
                      "and `texture` shows a texture it names";
    out["axes"] = "glTF's Y up is the game's Z; Blender's import turns it back to Z up";
    out["blender"] = "bpy.ops.import_scene.gltf(filepath=r\"" + PathText(gltfPath) +
                     "\"); the game's names and kinds are kept as custom properties (`dao`) for the way back";
    out["next"] = "after editing, export glTF from Blender with custom properties (export_extras=True) and give it "
                  "to `model_import`";
    return out;
}

// ---------------------------------------------------------------- model_import

Json Import(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "model_import");
    const fs::path to = OutputFolder(ghilana, args, "model_import");
    const fs::path from = fs::path(Wide(args.String("gltf")));
    GltfFile gltf;
    try
    {
        gltf = ReadGltf(from);
    }
    catch (const std::exception &error)
    {
        throw ToolError(std::string("model_import: ") + error.what());
    }
    const auto loaded = Load(ghilana, args, "model_import");
    Imported imported;
    try
    {
        imported = ModelFromGltf(gltf, loaded->tree, loaded->mesh);
    }
    catch (const std::exception &error)
    {
        throw ToolError(std::string("model_import: ") + error.what());
    }
    const model::Written mesh = model::WriteMesh(loaded->meshBytes, imported.chunks);
    if (!mesh.error.empty())
    {
        throw ToolError("model_import: the mesh file cannot be written: " + mesh.error);
    }
    const auto meshFile = gff4::File::Parse(mesh.bytes);
    const auto written = meshFile ? model::MeshOf(*meshFile) : std::nullopt;
    if (!written)
    {
        throw ToolError("model_import: the mesh file written does not read back; nothing was written");
    }
    const model::Written tree = model::WriteHierarchy(loaded->treeBytes, imported.edits, imported.added, &*written);
    if (!tree.error.empty())
    {
        throw ToolError("model_import: the model's hierarchy cannot be written: " + tree.error);
    }
    const fs::path meshPath = to / fs::path(Wide(loaded->meshName));
    const fs::path treePath = to / fs::path(Wide(loaded->treeName));
    WriteFile(meshPath, mesh.bytes, "model_import");
    WriteFile(treePath, tree.bytes, "model_import");

    Json out = {{"model", loaded->treeFrom}, {"mesh_file", loaded->meshFrom}};
    out["written"] = Json::array({PathText(meshPath), PathText(treePath)});
    out["chunks"] = imported.changes;
    Json edits = Json::array();
    for (const auto &edit : imported.edits)
    {
        Json item = {{"node", Utf8(loaded->tree.nodes[static_cast<std::size_t>(edit.node)].name)}};
        if (edit.material)
        {
            item["material"] = Utf8(*edit.material);
        }
        if (edit.bonesUsed)
        {
            item["bones_used"] = edit.bonesUsed->size();
        }
        edits.push_back(std::move(item));
    }
    out["node_edits"] = std::move(edits);
    Json added = Json::array();
    for (const auto &node : imported.added)
    {
        added.push_back({{"node", Utf8(node.name)},
                         {"chunk", Utf8(node.mesh.chunk)},
                         {"material", Utf8(node.mesh.material)},
                         {"like", Utf8(loaded->tree.nodes[static_cast<std::size_t>(node.like)].name)}});
    }
    out["nodes_added"] = std::move(added);
    if (!imported.notes.empty())
    {
        out["notes"] = imported.notes;
    }
    out["not_taken"] = "node transforms (bones moved in Blender), bones added, nodes removed, and textures: the files "
                       "keep the original's";
    out["into_the_game"] = "the game takes a loose file in " +
                           PathText(ghilana.Files().Where().documents / L"packages" / L"core" / L"override") +
                           " ahead of its archives (`conflicts` shows the copies); copy both files there, and take "
                           "them out to undo";
    return out;
}

} // namespace

void RegisterModelTools(Bench &bench, Ghilana &ghilana)
{
    auto &tools = bench.Tools();
    Json modelProperties = kModelProperties;
    modelProperties["tree"] = {{"type", "boolean"},
                               {"description", "List every node too: its name, kind, parent and bone index. Default "
                                               "false."}};
    tools.Add(Spec("model",
                   "Says what a model is made of, from its hierarchy (.mmh) and the mesh file (.msh) it names: its "
                   "nodes by kind, its bones, and each mesh node's chunk with its material (.mao), vertex and "
                   "triangle counts, vertex elements, bounds, and whether it is skinned and over how many bones. "
                   "`tree` lists the nodes. Use `model_export` to open it in Blender.",
                   std::move(modelProperties), {"name"}),
              [&ghilana](const Json &args) -> ToolResult { return Model(ghilana, args); });

    Json exportProperties = kModelProperties;
    exportProperties["to"] = {
        {"type", "string"},
        {"description", "The folder to write into, a full path; made if missing. Never inside the game's folder."}};
    tools.Add(Spec("model_export",
                   "Writes a model as glTF 2.0 (<name>.gltf and <name>.bin) into a folder of yours, for Blender's "
                   "built-in glTF import: its node tree, each mesh with normals, tangents, texture coordinates and "
                   "colours, and its bones as one skin with each vertex's weights. Z up is made glTF's Y up. The "
                   "game's names and kinds are kept in each node's and mesh's `extras` (\"dao\"), which Blender keeps "
                   "as custom properties. Answers the paths, the counts, and `notes` on anything left out. Textures "
                   "are not written yet. Writes nothing in the game's folders.",
                   std::move(exportProperties), {"name", "to"}, false),
              [&ghilana](const Json &args) -> ToolResult { return Export(ghilana, args); });

    Json importProperties = kModelProperties;
    importProperties["name"]["description"] = "The model the glTF was made from, its hierarchy (.mmh): the one "
                                              "`model_export` wrote it from. Its files are the base the meshes are "
                                              "written into.";
    importProperties["gltf"] = {{"type", "string"},
                                {"description", "The edited model, a .gltf (with its .bin beside it) or .glb, full "
                                                "path. Export from Blender with custom properties on "
                                                "(export_extras=True), so each mesh still says its chunk."}};
    importProperties["to"] = {{"type", "string"},
                              {"description", "The folder to write the .msh and .mmh into, a full path; made if "
                                              "missing. Never inside the game's folder."}};
    tools.Add(Spec("model_import",
                   "Writes a model edited in Blender back as the game's files: the glTF's meshes put into the "
                   "original .msh in place of their chunks (matched by the custom properties `model_export` wrote, "
                   "else by name), each vertex where the glTF has it at rest, with its normal, tangent and binormal, "
                   "texture coordinates and its bones by name, and the .mmh with each mesh node's bones and material "
                   "made to match and its bounding boxes made again. Vertices and triangles may be added, moved or "
                   "removed. A mesh that is none of the model's chunks, or a copy of one (Blender's duplicate), is "
                   "added as a new chunk under a new mesh node like the one it was copied from (else the first), "
                   "with the glTF's material. Bones may not be added. A mesh weighted to more than 67 bones (the "
                   "most the game's skinning shaders take for one chunk) is split into chunks of at most 67, the "
                   "rest under new mesh nodes like its own. "
                   "Bone moves and textures are not taken. Answers each chunk's counts before and after, the files "
                   "written, `notes`, and where to put the files for the game to take them. Writes nothing in the "
                   "game's folders.",
                   std::move(importProperties), {"name", "gltf", "to"}, false),
              [&ghilana](const Json &args) -> ToolResult { return Import(ghilana, args); });
}

} // namespace eluvian::ghilana::detail
