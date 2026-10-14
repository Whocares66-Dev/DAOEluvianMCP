// A model as glTF 2.0, for `model_export` (#132): the first half of a round
// trip through Blender, whose glTF import and export are built in. glTF and
// not OBJ, since a creature's or an armour's mesh is skinned: glTF keeps the
// node tree, the bones and each vertex's weights.
//
// What DAOLib's core reads of the .mmh and .msh (dao/core/Model.h) is put in
// glTF's terms here: Z up made Y up, a chunk's blend indices (which count in
// its mesh node's bones) made indices into the model's one skin, and the
// file's own names and kinds kept as `extras` under "dao", which Blender
// keeps as custom properties, for the way back.
#pragma once

#include "core/Tool.h"

#include <dao/core/Model.h>

#include <cstddef>
#include <string>
#include <vector>

namespace eluvian::ghilana
{

struct Gltf
{
    // The .gltf's JSON. Its one buffer has no `uri`: the writer names the
    // .bin it writes beside it.
    Json document;
    // The buffer: every accessor's bytes.
    std::vector<std::byte> binary;
    // What was left out or changed, and why, in an answer's words.
    std::vector<std::string> notes;
};

// A vertex element's usage and format by Direct3D's names, less the D3DDECL
// prefix ("TexCoord", "Float16x2").
std::string UsageName(dao::core::model::Usage usage);
std::string FormatName(dao::core::model::Format format);

// The model's nodes in the file's order (glTF node i is the hierarchy's node
// i), each mesh node with its chunk as a mesh and its material named, and the
// model's bones as one skin when a mesh is skinned.
Gltf ModelGltf(const dao::core::model::Hierarchy &tree, const dao::core::model::Mesh &mesh);

} // namespace eluvian::ghilana
