// A model back from glTF, for `model_import` (#132): the second half of the
// round trip through Blender. What Blender (or anything) wrote is read
// against the model it came from, the game's own .mmh and .msh, and made
// what DAOLib's writers take (dao/core/Model.h, DAOLib #117): each chunk's
// new vertices in its own declaration, and the mesh nodes whose bones or
// material changed. The writers keep the rest of the files as they were.
//
// Meshes are matched to chunks by what the export put in their extras
// ("dao": chunk, chunk_index), which Blender keeps as custom properties, and
// failing that by the mesh's or its node's name. Bones are matched by name.
// A vertex is put back where the glTF has it in its rest pose, in the space
// of the mesh node it belongs to, whatever Blender did to the transforms
// between.
#pragma once

#include "core/Tool.h"

#include <dao/core/Model.h>

#include <cstddef>
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
    // Per chunk given back: its name, vertices and triangles before and after.
    Json changes = Json::array();
    // What was left out, made up or refused in part, in an answer's words.
    std::vector<std::string> notes;
};

// The chunks and node edits of a glTF made of this model. Throws
// std::runtime_error for a glTF none of whose meshes is one of the model's,
// or a mesh that cannot be written (a bone the model does not have).
Imported ModelFromGltf(const GltfFile &gltf, const dao::core::model::Hierarchy &tree,
                       const dao::core::model::Mesh &mesh);

} // namespace eluvian::ghilana
