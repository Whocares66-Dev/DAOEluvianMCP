#include "core/PictureSize.h"

#include "core/Tool.h"

#include <cmath>
#include <cstdlib>
#include <format>

namespace eluvian
{

namespace
{

// `side` times `numerator` over `denominator`, to the nearest pixel, and at least one.
int Scaled(int side, int numerator, int denominator)
{
    const auto exact = static_cast<double>(side) * numerator / denominator;
    return std::max(1, static_cast<int>(std::lround(exact)));
}

void CheckSide(const char *name, std::optional<int> side)
{
    if (side && (*side < kLeastSide || *side > kMostSide))
    {
        throw ToolError(std::format("screenshot: `{}` is {} to {} pixels", name, kLeastSide, kMostSide));
    }
}

} // namespace

PictureFit FitPicture(int width, int height, std::optional<int> askedWidth, std::optional<int> askedHeight, bool crop)
{
    CheckSide("width", askedWidth);
    CheckSide("height", askedHeight);
    PictureFit fit{0, 0, width, height, width, height};
    if (askedWidth && !askedHeight)
    {
        fit.width = *askedWidth;
        fit.height = Scaled(*askedWidth, height, width);
        return fit;
    }
    if (askedHeight && !askedWidth)
    {
        fit.width = Scaled(*askedHeight, width, height);
        fit.height = *askedHeight;
        return fit;
    }
    if (!askedWidth)
    {
        return fit;
    }
    fit.width = *askedWidth;
    fit.height = *askedHeight;
    // The frame's aspect to a pixel: a height the width gives, rounded, may
    // be one off the one another renderer was sized to.
    if (std::abs(Scaled(*askedWidth, height, width) - *askedHeight) <= 1)
    {
        return fit;
    }
    if (!crop)
    {
        throw ToolError(std::format("screenshot: {} by {} is not the frame's aspect ({} by {}), and the frame is "
                                    "not stretched: give one side ({} by {} or {} by {} keep it), or pass `crop` "
                                    "true for the middle of the frame at that aspect",
                                    *askedWidth, *askedHeight, width, height, *askedWidth,
                                    Scaled(*askedWidth, height, width), Scaled(*askedHeight, width, height),
                                    *askedHeight));
    }
    // Wider than asked: the whole height and a middle part of the width; else the other way.
    if (static_cast<long long>(width) * *askedHeight > static_cast<long long>(height) * *askedWidth)
    {
        fit.cropWidth = std::min(width, Scaled(height, *askedWidth, *askedHeight));
        fit.cropX = (width - fit.cropWidth) / 2;
    }
    else
    {
        fit.cropHeight = std::min(height, Scaled(width, *askedHeight, *askedWidth));
        fit.cropY = (height - fit.cropHeight) / 2;
    }
    return fit;
}

} // namespace eluvian
