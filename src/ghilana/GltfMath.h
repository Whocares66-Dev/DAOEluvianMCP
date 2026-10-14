// The vectors, turns and matrices of a model, between the game's space (Z
// up) and glTF's (Y up), for the export (Gltf.cpp) and the import
// (GltfImport.cpp). A matrix is affine, kept column-major as glTF keeps one.
#pragma once

#include <dao/core/Model.h>

#include <array>
#include <vector>

namespace eluvian::ghilana::math
{

using Vec3 = std::array<float, 3>;
using Mat4 = std::array<double, 16>;

// A point or direction of the game's in glTF's axes: (x, y, z) is (x, z, -y).
Vec3 Axes(float x, float y, float z);
// A turn of the game's in glTF's axes, of length 1.
dao::core::model::Quaternion Axes(const dao::core::model::Quaternion &q);
// A point or direction of glTF's in the game's axes: Axes undone.
Vec3 Back(const Vec3 &v);

// `v` of length 1, or `otherwise` for one of length 0.
Vec3 Normalised(Vec3 v, Vec3 otherwise);
Vec3 Cross(const Vec3 &a, const Vec3 &b);
float Dot(const Vec3 &a, const Vec3 &b);

// The matrix of a translation, a turn of length 1 and a uniform scale, in that order outward.
Mat4 Transform(const Vec3 &t, const dao::core::model::Quaternion &q, float s);
// `a` after `b`.
Mat4 Times(const Mat4 &a, const Mat4 &b);
// The inverse of an affine matrix; the identity for one that has none.
Mat4 Inverse(const Mat4 &m);

// The turn of an affine matrix, its scale taken out, as a quaternion.
dao::core::model::Quaternion Rotation(const Mat4 &m);
// A point moved by a matrix.
Vec3 Apply(const Mat4 &m, const Vec3 &v);
// A direction turned (and scaled) by a matrix, its translation left out.
Vec3 Turn(const Mat4 &m, const Vec3 &v);

// A node's own transform in glTF's axes: its translation, turn and scale.
Mat4 Local(const dao::core::model::Node &node);
// Each node's place in the model, in glTF's axes: its parents' transforms and its own.
std::vector<Mat4> Worlds(const dao::core::model::Hierarchy &tree);

} // namespace eluvian::ghilana::math
