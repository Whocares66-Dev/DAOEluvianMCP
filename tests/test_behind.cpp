#include "core/Behind.h"

#include <catch2/catch_test_macros.hpp>

using namespace eluvian;

TEST_CASE("a copy kept behind shows its window without activating it", "[behind]")
{
    // winuser.h: SW_SHOWNORMAL 1, SW_SHOWMINIMIZED 2, SW_SHOWMAXIMIZED 3,
    // SW_SHOWNOACTIVATE 4, SW_SHOW 5, SW_SHOWMINNOACTIVE 7, SW_SHOWNA 8,
    // SW_RESTORE 9.
    CHECK(BehindShowCommand(1) == 4);
    CHECK(BehindShowCommand(9) == 4);
    CHECK(BehindShowCommand(2) == 7);
    CHECK(BehindShowCommand(5) == 8);
    CHECK(BehindShowCommand(3) == 8);
}

TEST_CASE("a copy kept behind leaves a command that does not activate as it is", "[behind]")
{
    // SW_HIDE 0, SW_SHOWNOACTIVATE 4, SW_MINIMIZE 6, SW_SHOWMINNOACTIVE 7,
    // SW_SHOWNA 8, SW_SHOWDEFAULT 10 (the launch's SW_SHOWNOACTIVATE),
    // SW_FORCEMINIMIZE 11.
    for (const int command : {0, 4, 6, 7, 8, 10, 11})
    {
        CHECK(BehindShowCommand(command) == command);
    }
}

TEST_CASE("a copy kept behind is never activated or raised by its own SetWindowPos", "[behind]")
{
    // SWP_NOSIZE 0x1, SWP_NOMOVE 0x2, SWP_NOZORDER 0x4, SWP_NOACTIVATE 0x10,
    // SWP_SHOWWINDOW 0x40.
    CHECK(BehindPositionFlags(0) == 0x14);
    CHECK(BehindPositionFlags(0x1 | 0x2 | 0x40) == (0x1 | 0x2 | 0x40 | 0x14));
    CHECK(BehindPositionFlags(0x14) == 0x14);
}
