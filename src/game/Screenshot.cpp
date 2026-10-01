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
    throw ToolError(std::format("screenshot: the game wrote no picture to {} within 10 s; source \"window\" "
                                "draws the window instead",
                                folder.string()));
}

} // namespace

void AddScreenshot(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        ToolSpec{"screenshot",
                 "A picture of the game as it is now, interface and all; no window over the game gets in the "
                 "way. `source`: window (the default) has Windows draw the game's window from inside, at once, "
                 "even while frames stop; game has the game take its own, as its screenshot key does, a frame "
                 "or more later. Answers the picture as an image, with its size and file (Screenshots\\Eluvian "
                 "or the game's Screenshots\\User); `image: false` answers the file alone, to save tokens; "
                 "`quality` from 0.1 to 1 (window only), 0.8 by default.",
                 Json{{"type", "object"},
                      {"properties",
                       {{"source", {{"type", "string"}, {"enum", {"window", "game"}}}},
                        {"image", {{"type", "boolean"}, {"description", "Answer the picture itself; default true"}}},
                        {"quality", {{"type", "number"}, {"description", "JPEG quality, 0.1 to 1; default 0.8"}}}}}},
                 Thread::Listener, true},
        [&bench, &watch](const Json &raw) -> ToolResult {
            const Args args(raw, "screenshot");
            const auto source = args.Choice("source", {"window", "game"}, "window");
            const bool image = args.Bool("image", true);
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
                const auto picture = ReencodeJpeg(bytes, 0.9F);
                const auto folder = Screenshots() / "Eluvian";
                std::error_code error;
                std::filesystem::create_directories(folder, error);
                file = folder / ("Game" + original.filename().string());
                std::ofstream(file, std::ios::binary)
                    .write(picture.jpeg.data(), static_cast<std::streamsize>(picture.jpeg.size()));
                answer["width"] = picture.width;
                answer["height"] = picture.height;
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
                const auto picture = CaptureGameWindow(static_cast<float>(quality));
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
