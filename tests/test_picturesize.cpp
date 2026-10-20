#include "core/PictureSize.h"
#include "core/Tool.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace eluvian;
using Catch::Matchers::ContainsSubstring;

namespace
{

void Same(const PictureFit &fit, const PictureFit &expected)
{
    CHECK(fit.cropX == expected.cropX);
    CHECK(fit.cropY == expected.cropY);
    CHECK(fit.cropWidth == expected.cropWidth);
    CHECK(fit.cropHeight == expected.cropHeight);
    CHECK(fit.width == expected.width);
    CHECK(fit.height == expected.height);
}

} // namespace

TEST_CASE("A picture asked no size is the frame as it is", "[picturesize]")
{
    Same(FitPicture(1600, 1000, std::nullopt, std::nullopt, false), {0, 0, 1600, 1000, 1600, 1000});
    Same(FitPicture(1600, 1000, std::nullopt, std::nullopt, true), {0, 0, 1600, 1000, 1600, 1000});
}

TEST_CASE("One side asked keeps the frame's aspect", "[picturesize]")
{
    Same(FitPicture(1600, 1000, 800, std::nullopt, false), {0, 0, 1600, 1000, 800, 500});
    Same(FitPicture(1600, 1000, std::nullopt, 720, false), {0, 0, 1600, 1000, 1152, 720});
    // Rounded to the nearest pixel, larger as well as smaller.
    Same(FitPicture(1680, 1050, 1000, std::nullopt, false), {0, 0, 1680, 1050, 1000, 625});
    Same(FitPicture(1600, 1000, 2001, std::nullopt, false), {0, 0, 1600, 1000, 2001, 1251});
}

TEST_CASE("Both sides of the frame's aspect, to a pixel, scale the whole frame", "[picturesize]")
{
    Same(FitPicture(1600, 1000, 800, 500, false), {0, 0, 1600, 1000, 800, 500});
    // 1000 by 626 is a pixel off 625: another renderer sized to it compares.
    Same(FitPicture(1680, 1050, 1000, 626, false), {0, 0, 1680, 1050, 1000, 626});
}

TEST_CASE("Both sides of another aspect are refused, naming the sizes that keep it", "[picturesize]")
{
    REQUIRE_THROWS_AS(FitPicture(1600, 1000, 1280, 720, false), ToolError);
    REQUIRE_THROWS_WITH(FitPicture(1600, 1000, 1280, 720, false),
                        ContainsSubstring("1280 by 800 or 1152 by 720") && ContainsSubstring("`crop`"));
}

TEST_CASE("With crop, the middle of the frame at the asked aspect is scaled", "[picturesize]")
{
    // A wider aspect than the frame's: the whole width, a middle band of the height.
    Same(FitPicture(1600, 1000, 1280, 720, true), {0, 50, 1600, 900, 1280, 720});
    // A narrower one: the whole height, a middle band of the width.
    Same(FitPicture(1600, 1000, 1000, 1000, true), {300, 0, 1000, 1000, 1000, 1000});
    // Of the frame's aspect, crop changes nothing.
    Same(FitPicture(1600, 1000, 800, 500, true), {0, 0, 1600, 1000, 800, 500});
}

TEST_CASE("A side out of bounds is refused", "[picturesize]")
{
    REQUIRE_THROWS_WITH(FitPicture(1600, 1000, 15, std::nullopt, false), ContainsSubstring("`width` is 16 to 8192"));
    REQUIRE_THROWS_WITH(FitPicture(1600, 1000, std::nullopt, 8193, false), ContainsSubstring("`height` is 16 to 8192"));
    REQUIRE_NOTHROW(FitPicture(1600, 1000, 16, std::nullopt, false));
    REQUIRE_NOTHROW(FitPicture(1600, 1000, std::nullopt, 8192, false));
}
