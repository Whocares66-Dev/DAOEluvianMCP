// The vectors, turns and matrices of a model between the game's space and
// glTF's (GltfMath.h).
#include "ghilana/GltfMath.h"

#include <cmath>
#include <utility>

namespace eluvian::ghilana::math
{

namespace model = dao::core::model;

// The game's models are Z up, glTF's Y up: (x, y, z) is (x, z, -y), a turn
// about x, so nothing is mirrored and a triangle's winding stays.
Vec3 Axes(float x, float y, float z)
{
    return {x, z, -y};
}

model::Quaternion Axes(const model::Quaternion &q)
{
    const Vec3 v = Axes(q[0], q[1], q[2]);
    model::Quaternion out{v[0], v[1], v[2], q[3]};
    const double length = std::sqrt((double{out[0]} * out[0]) + (double{out[1]} * out[1]) + (double{out[2]} * out[2]) +
                                    (double{out[3]} * out[3]));
    if (length < 1e-6)
    {
        return {0, 0, 0, 1};
    }
    for (float &c : out)
    {
        c = static_cast<float>(c / length);
    }
    return out;
}

Vec3 Normalised(Vec3 v, Vec3 otherwise)
{
    const double length = std::sqrt((double{v[0]} * v[0]) + (double{v[1]} * v[1]) + (double{v[2]} * v[2]));
    if (length < 1e-12)
    {
        return otherwise;
    }
    for (float &c : v)
    {
        c = static_cast<float>(c / length);
    }
    return v;
}

Vec3 Cross(const Vec3 &a, const Vec3 &b)
{
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]), (a[0] * b[1]) - (a[1] * b[0])};
}

float Dot(const Vec3 &a, const Vec3 &b)
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Mat4 Transform(const Vec3 &t, const model::Quaternion &q, float s)
{
    const double x = q[0];
    const double y = q[1];
    const double z = q[2];
    const double w = q[3];
    Mat4 m{};
    m[0] = (1 - (2 * ((y * y) + (z * z)))) * s;
    m[1] = 2 * ((x * y) + (z * w)) * s;
    m[2] = 2 * ((x * z) - (y * w)) * s;
    m[4] = 2 * ((x * y) - (z * w)) * s;
    m[5] = (1 - (2 * ((x * x) + (z * z)))) * s;
    m[6] = 2 * ((y * z) + (x * w)) * s;
    m[8] = 2 * ((x * z) + (y * w)) * s;
    m[9] = 2 * ((y * z) - (x * w)) * s;
    m[10] = (1 - (2 * ((x * x) + (y * y)))) * s;
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1;
    return m;
}

Mat4 Times(const Mat4 &a, const Mat4 &b)
{
    Mat4 c{};
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            double sum = 0;
            for (int k = 0; k < 4; ++k)
            {
                sum += a[(k * 4) + row] * b[(col * 4) + k];
            }
            c[(col * 4) + row] = sum;
        }
    }
    return c;
}

// The inverse of an affine matrix: its 3x3 part's, and the translation
// turned back.
Mat4 Inverse(const Mat4 &m)
{
    const auto at = [&](int row, int col) { return m[(col * 4) + row]; };
    const double c00 = (at(1, 1) * at(2, 2)) - (at(1, 2) * at(2, 1));
    const double c01 = (at(1, 2) * at(2, 0)) - (at(1, 0) * at(2, 2));
    const double c02 = (at(1, 0) * at(2, 1)) - (at(1, 1) * at(2, 0));
    const double det = (at(0, 0) * c00) + (at(0, 1) * c01) + (at(0, 2) * c02);
    if (std::abs(det) < 1e-12)
    {
        return Transform({0, 0, 0}, {0, 0, 0, 1}, 1);
    }
    const double d = 1 / det;
    std::array<double, 9> r{};
    r[0] = c00 * d;
    r[1] = ((at(0, 2) * at(2, 1)) - (at(0, 1) * at(2, 2))) * d;
    r[2] = ((at(0, 1) * at(1, 2)) - (at(0, 2) * at(1, 1))) * d;
    r[3] = c01 * d;
    r[4] = ((at(0, 0) * at(2, 2)) - (at(0, 2) * at(2, 0))) * d;
    r[5] = ((at(0, 2) * at(1, 0)) - (at(0, 0) * at(1, 2))) * d;
    r[6] = c02 * d;
    r[7] = ((at(0, 1) * at(2, 0)) - (at(0, 0) * at(2, 1))) * d;
    r[8] = ((at(0, 0) * at(1, 1)) - (at(0, 1) * at(1, 0))) * d;
    Mat4 out{};
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
        {
            out[(col * 4) + row] = r[(row * 3) + col];
        }
        out[12 + row] = -((r[row * 3] * m[12]) + (r[(row * 3) + 1] * m[13]) + (r[(row * 3) + 2] * m[14]));
    }
    out[15] = 1;
    return out;
}

Vec3 Back(const Vec3 &v)
{
    return {v[0], -v[2], v[1]};
}

Vec3 Apply(const Mat4 &m, const Vec3 &v)
{
    return {static_cast<float>((m[0] * v[0]) + (m[4] * v[1]) + (m[8] * v[2]) + m[12]),
            static_cast<float>((m[1] * v[0]) + (m[5] * v[1]) + (m[9] * v[2]) + m[13]),
            static_cast<float>((m[2] * v[0]) + (m[6] * v[1]) + (m[10] * v[2]) + m[14])};
}

Vec3 Turn(const Mat4 &m, const Vec3 &v)
{
    return {static_cast<float>((m[0] * v[0]) + (m[4] * v[1]) + (m[8] * v[2])),
            static_cast<float>((m[1] * v[0]) + (m[5] * v[1]) + (m[9] * v[2])),
            static_cast<float>((m[2] * v[0]) + (m[6] * v[1]) + (m[10] * v[2]))};
}

Mat4 Local(const model::Node &node)
{
    return Transform(Axes(node.translation[0], node.translation[1], node.translation[2]), Axes(node.rotation),
                     node.scale.value_or(1.0F));
}

std::vector<Mat4> Worlds(const model::Hierarchy &tree)
{
    std::vector<Mat4> world(tree.nodes.size());
    for (std::size_t i = 0; i < tree.nodes.size(); ++i)
    {
        const model::Node &node = tree.nodes[i];
        const Mat4 local = Local(node);
        // Each node comes after its parent in the file's order (Model.h).
        world[i] = node.parent >= 0 && std::cmp_less(node.parent, i)
                       ? Times(world[static_cast<std::size_t>(node.parent)], local)
                       : local;
    }
    return world;
}

} // namespace eluvian::ghilana::math
