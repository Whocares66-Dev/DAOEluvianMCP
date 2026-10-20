// `screenshot`: the game as a picture, its window drawn from inside the
// process (game/Capture.h) or the game's own, as its key takes it.
#include "game/Shared.h"

#include "core/Base64.h"
#include "game/Capture.h"

#include <Windows.h>

namespace eluvian::game
{

namespace
{

// Where the pictures go, beside the game's logs: <Documents>\BioWare\
// Dragon Age\Screenshots, Eluvian's own in Eluvian, the game's in User.
std::filesystem::path Screenshots()
{
    return dao::LogDirectory().parent_path() / "Screenshots";
}

// The newest .jpg in a folder.
std::optional<std::filesystem::path> NewestJpeg(const std::filesystem::path &folder)
{
    std::error_code error;
    std::optional<std::filesystem::path> newest;
    std::filesystem::file_time_type newestTime{};
    for (const auto &entry : std::filesystem::directory_iterator(folder, error))
    {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".jpg")
        {
            continue;
        }
        const auto written = entry.last_write_time(error);
        if (!newest || written > newestTime)
        {
            newest = entry.path();
            newestTime = written;
        }
    }
    return newest;
}

// The game's own picture: the message its screenshot key sends
// (TakeScreenshotMessage, to the mode controller; DAOLib f253b8c), which
// has the renderer write a JPEG, interface and all, to Screenshots\User a
// frame or more later. Sent on the frame thread, waited for here.
//
// It can crash the game, and not by anything sent here: the message has no
// name in it. The engine makes the picture's file name itself and, handing
// it to its render thread (0x00A23D1D..0x00A23D44), copies a fixed 0x400
// bytes out of that string's buffer whatever its length; a buffer within
// 1 KB of an unmapped page is read past (elgara-agent's crash, 2026-10-11
// 11:56, reading 0x4FCA6000; #120). The screenshot key did the same. The
// fix is to the game and is the loader's: DAOLib #78 (fd43f73, the loader
// of every game Eluvian's next release runs in) hands the renderer the name
// in a buffer of the full 0x400 bytes. Under a loader from before it, the
// 0.1.0 one among them, the game's own screenshot can still crash.
std::filesystem::path GameScreenshot(Bench &bench, const Watch &watch)
{
    const auto folder = Screenshots() / "User";
    const auto before = NewestJpeg(folder);
    bench.Frames().RunAndWait(
        [&watch] {
            RequireWorld(watch, "screenshot");
            auto *app = dao::CClientExoApp::Get();
            auto *modes = app != nullptr ? app->GameModes() : nullptr;
            if (modes == nullptr)
            {
                throw ToolError("screenshot: the game's modes are not up yet");
            }
            dao::messages::Send(modes, dao::messages::Make<dao::messages::TakeScreenshotMessage>());
        },
        std::chrono::milliseconds(5000));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::uintmax_t lastSize = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto newest = NewestJpeg(folder);
        if (!newest || newest == before)
        {
            continue;
        }
        // Written when its size holds still between two looks.
        std::error_code error;
        const auto size = std::filesystem::file_size(*newest, error);
        if (!error && size > 0 && size == lastSize)
        {
            return *newest;
        }
        lastSize = error ? 0 : size;
    }
    if (GameWindowPlace().minimised)
    {
        throw ToolError(std::format("screenshot: the game wrote no picture to {} within 10 s, and {}", folder.string(),
                                    kMinimised));
    }
    throw ToolError(std::format("screenshot: the game wrote no picture to {} within 10 s; source \"window\" "
                                "draws the window instead",
                                folder.string()));
}

} // namespace

void AddScreenshot(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{
            "screenshot",
            "Takes a picture of the game as it is now, interface included. Other windows on top of the game "
            "do not get in the way. `source`:\n\n"
            "- `window` (the default): Windows draws the game's window at once, even while the game is not "
            "drawing frames. If the display is asleep or the session locked, the picture is all black "
            "(`black`); use `game` then.\n"
            "- `game`: the game takes its own screenshot, as its screenshot key does, a frame or more later. "
            "While another plugin's free view flies (a photo camera's), the game hides its interface and "
            "that plugin keeps its own panel out of this picture, so it is the frame alone.\n\n"
            "Returns the picture as an image, with its `width`, `height` and `file` (in Screenshots\\Eluvian; "
            "for `game`, the game's own file is `original`). Pass `image: false` to get only the file and "
            "save tokens. `width` or `height` scales the picture, the other side kept to the frame's aspect; "
            "both must be the frame's aspect, or with `crop` true the middle of the frame at theirs is "
            "taken. The game draws at its window's size, so a larger picture is no sharper: `frame` is the "
            "size it was drawn at. To draw the same frame elsewhere, `camera`'s `projection` is the one it "
            "was drawn with.",
            Json{{"type", "object"},
                 {"properties",
                  {{"source",
                    {{"type", "string"},
                     {"enum", {"window", "game"}},
                     {"description", "Who takes the picture. Default window."}}},
                   {"image",
                    {{"type", "boolean"}, {"description", "Include the picture itself in the answer. Default true."}}},
                   {"quality",
                    {{"type", "number"}, {"description", "With `window`: JPEG quality, 0.1 to 1. Default 0.8."}}},
                   {"width",
                    {{"type", "integer"},
                     {"description", "The picture's width in pixels, 16 to 8192. Default: the frame's."}}},
                   {"height",
                    {{"type", "integer"},
                     {"description", "The picture's height in pixels, 16 to 8192. Default: the frame's."}}},
                   {"crop",
                    {{"type", "boolean"},
                     {"description", "With both `width` and `height` of another aspect than the frame's: take the "
                                     "middle of the frame at theirs. Default false: refused."}}}}}},
            Thread::Listener, true},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "screenshot");
            const auto source = args.Choice("source", {"window", "game"}, "window");
            const bool image = args.Bool("image", true);
            SizeAsked size;
            if (args.Has("width"))
            {
                size.width = static_cast<int>(args.Integer("width", 0, kLeastSide, kMostSide));
            }
            if (args.Has("height"))
            {
                size.height = static_cast<int>(args.Integer("height", 0, kLeastSide, kMostSide));
            }
            size.crop = args.Bool("crop", false);
            std::filesystem::path file;
            std::string jpeg;
            Json answer = {{"source", source}};
            if (source == "game")
            {
                // The game's file as it wrote it, and the picture written again
                // plain beside Eluvian's own: the game's JPEG has an Exif header
                // some readers will not take.
                const auto original = GameScreenshot(bench, watch);
                std::string bytes;
                {
                    std::ifstream in(original, std::ios::binary);
                    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                }
                const auto picture = ReencodeJpeg(bytes, 0.9F, size);
                const auto folder = Screenshots() / "Eluvian";
                std::error_code error;
                std::filesystem::create_directories(folder, error);
                file = folder / ("Game" + original.filename().string());
                std::ofstream(file, std::ios::binary)
                    .write(picture.jpeg.data(), static_cast<std::streamsize>(picture.jpeg.size()));
                answer["width"] = picture.width;
                answer["height"] = picture.height;
                answer["frame"] = {{"width", picture.frameWidth}, {"height", picture.frameHeight}};
                answer["original"] = original.string();
                jpeg = picture.jpeg;
            }
            else
            {
                const double quality = args.Number("quality", 0.8);
                if (quality < 0.1 || quality > 1)
                {
                    throw ToolError("screenshot: `quality` is from 0.1 to 1");
                }
                const auto picture = CaptureGameWindow(static_cast<float>(quality), size);
                const auto folder = Screenshots() / "Eluvian";
                std::error_code error;
                std::filesystem::create_directories(folder, error);
                SYSTEMTIME now{};
                GetLocalTime(&now);
                file = folder / std::format("Eluvian{:04}{:02}{:02}{:02}{:02}{:02}{:03}.jpg", now.wYear, now.wMonth,
                                            now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
                std::ofstream(file, std::ios::binary)
                    .write(picture.jpeg.data(), static_cast<std::streamsize>(picture.jpeg.size()));
                answer["width"] = picture.width;
                answer["height"] = picture.height;
                answer["frame"] = {{"width", picture.frameWidth}, {"height", picture.frameHeight}};
                if (picture.black)
                {
                    answer["black"] = true;
                    answer["note"] = "the picture is black all over: Windows drew nothing of the game's window, "
                                     "as when the display is asleep or the session locked. `source`: \"game\" "
                                     "takes the game's own frame, which does not depend on the desktop";
                }
                jpeg = picture.jpeg;
            }
            answer["file"] = file.string();
            answer["bytes"] = jpeg.size();
            ToolResult result(std::move(answer));
            if (image)
            {
                result.content.push_back(Json{{"type", "image"}, {"data", Base64(jpeg)}, {"mimeType", "image/jpeg"}});
            }
            return result;
        });
}

} // namespace eluvian::game
