// A model back from glTF, for `model_import` (#132): the second half of the
// round trip through Blender. What Blender (or anything) wrote is read
// against the model it came from, the game's own .mmh and .msh, and made
// what DAOLib's writers take (dao/core/Model.h, DAOLib #117): each chunk's
// new vertices in its own declaration, and the mesh nodes whose bones or
// material changed. The writers keep the rest of the files as they were.
//
// Meshes are matched to chunks by what the export put in their extras
// ("dao": chunk, chunk_index), which Blender keeps as custom properties, and
// failing that by the mesh's or its node's name. A mesh that is none of the
// chunks, or a second one claiming a chunk (Blender's duplicate keeps the
// extras), is a new chunk with a new mesh node, like the chunk it claims or
// else the first one drawn (#136). Bones are matched by name.
// A vertex is put back where the glTF has it in its rest pose, in the space
// of the mesh node it belongs to, whatever Blender did to the transforms
// between.
#pragma once

#include "core/Tool.h"

#include <dao/core/Model.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace eluvian::ghilana
{

// A glTF file read: its JSON and its buffers' bytes. A .glb's own buffer is
// its BIN chunk; a .gltf's are the files its buffers' uris name beside it.
struct GltfFile
{
    Json document;
    std::vector<std::vector<std::byte>> buffers;
};

// Reads a .gltf or .glb. Throws std::runtime_error saying what is wrong.
GltfFile ReadGltf(const std::filesystem::path &path);

struct Imported
{
    std::vector<dao::core::model::ChunkData> chunks;
    std::vector<dao::core::model::NodeEdit> edits;
    // The nodes added: first the new bones, a joint of the glTF's skin that
    // is no bone of the model each (#137, DAOLib #119), then the mesh nodes
    // of the chunks added, each like the mesh node of the chunk its mesh was
    // made from (#136, DAOLib #118). A new node's index in the hierarchy is
    // the model's node count and its place here.
    std::vector<dao::core::model::NewNode> added;
    // Per chunk given back: its name, vertices and triangles before and after.
    Json changes = Json::array();
    // What was left out, made up or refused in part, in an answer's words.
    std::vector<std::string> notes;
};

// A chunk's data and the bones its blend indices count in.
struct BonePiece
{
    dao::core::model::ChunkData data;
    std::vector<std::int32_t> bones;
};

// A chunk whose blend indices (slots in `bonesUsed`) count in more than
// `most` bones, cut into pieces of at most `most`, each a chunk's data with
// its own bones: a triangle goes to the first piece its bones fit in. The
// first piece keeps the chunk's name and index.
std::vector<BonePiece> SplitByBones(const dao::core::model::ChunkData &whole,
                                    const std::vector<dao::core::model::Element> &declaration,
                                    const std::vector<std::int32_t> &bonesUsed, std::size_t most);

// The chunks and node edits of a glTF made of this model. Throws
// std::runtime_error for a glTF none of whose meshes is one of the model's,
// or a mesh that cannot be written (a bone the model does not have). A mesh
// over kMostBonesInChunk bones is split (SplitByBones).
Imported ModelFromGltf(const GltfFile &gltf, const dao::core::model::Hierarchy &tree,
                       const dao::core::model::Mesh &mesh);

} // namespace eluvian::ghilana
