// The game's window as a picture, taken from inside the process. Windows
// draws the window into a bitmap itself (PrintWindow with
// PW_RENDERFULLCONTENT, which composes Direct3D's frame too), so a window
// over the game does not spoil the picture, as a capture of the screen from
// outside would: in the first play test a dialog sat over the game for ten
// minutes (2026-09-30). The game's own Screenshot script command writes
// nothing in this build (play, 2026-09-30); the key's way is not read yet.
#pragma once

#include <optional>
#include <string>

namespace eluvian::game
{

struct Picture
{
    std::string jpeg; // the bytes of a JPEG file
    int width = 0;
    int height = 0;
    // Every pixel black: Windows drew nothing of the game. Seen with the
    // display asleep, 2026-10-01, where the game's own screenshot of the
    // same moment was whole.
    bool black = false;
};

// The game's main window's client area, as a JPEG of `quality` (0 to 1).
// Throws ToolError saying why not: no window yet, minimised, or Windows
// could not draw it.
Picture CaptureGameWindow(float quality);

// Where the game's window is on the desktop: what turns a place in the
// game's picture into one a real mouse can be sent to.
struct WindowPlace
{
    bool found = false; // the game has a window
    int x = 0;          // the client area's top left, in desktop pixels
    int y = 0;
    int width = 0; // the client area's size
    int height = 0;
    bool minimised = false;
    bool foreground = false; // it is the window the keyboard and mouse go to
};
WindowPlace GameWindowPlace();

// What a real mouse at a desktop pixel would reach instead of the game: the
// title (or class) of the window on top of the game's there, or nothing
// where the game is on top. A desktop app over part of the game took a
// tester's clicks at a pixel the game's own pick had right, and the
// game's cursor saw nothing (computer-use-agent, 2026-10-02).
std::optional<std::string> CoveredAt(int x, int y);

// A JPEG decoded and written again as a plain one. The game's own
// screenshots carry an Exif header that some readers will not take: an
// image reader rejected one until it was saved again, and so did the
// model's own (2026-10-01). Throws ToolError if the bytes are no picture.
Picture ReencodeJpeg(const std::string &jpeg, float quality);

} // namespace eluvian::game
