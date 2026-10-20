// The size `screenshot` writes a picture at, apart from the game so a test
// holds the arithmetic. Comparing the game's frame with another renderer's
// (DAOParagonEngine's parity work, #156) needed both at one size, and each
// pair was scaled by hand (2026-10-19). The game draws at its window's size,
// so a picture of another size is the frame scaled, or cropped about its
// middle and scaled: never stretched, since a stretched frame compares with
// nothing.
#pragma once

#include <optional>

namespace eluvian
{

// The part of the frame a picture takes, in the frame's pixels, and the size
// it is written at.
struct PictureFit
{
    int cropX = 0;
    int cropY = 0;
    int cropWidth = 0;
    int cropHeight = 0;
    int width = 0;
    int height = 0;
};

// The fewest and most pixels a side may be asked for.
inline constexpr int kLeastSide = 16;
inline constexpr int kMostSide = 8192;

// How a frame of `width` by `height` is written at the size asked: neither
// side, the frame as it is; one, the other kept to the frame's aspect; both,
// at that size if it is the frame's aspect (to a pixel), else, with `crop`,
// the largest part of the frame of the asked aspect about its middle, scaled.
// Throws ToolError (screenshot's) for a side out of bounds, or two of
// another aspect without `crop`, naming the sizes that keep the frame's.
PictureFit FitPicture(int width, int height, std::optional<int> askedWidth, std::optional<int> askedHeight, bool crop);

} // namespace eluvian
