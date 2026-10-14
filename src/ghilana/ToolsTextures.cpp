// Ghilana's tools for a texture's round trip through an image editor
// (#135): a texture written out as a PNG (texture_export), and a PNG written
// back as the game's DDS (texture_import), in the original's format with its
// every halving, through DAOLib's core (dao/core/Dds.h, DAOLib #120). A
// normal map goes out with x, y, z in red, green and blue, as an editor
// shows one, and comes back as the game keeps it, x in alpha.
#include "ghilana/Ghilana.h"

#include "core/Args.h"
#include "ghilana/Png.h"
#include "ghilana/Text.h"

#include <dao/core/Dds.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>

namespace eluvian::ghilana::detail
{

namespace
{

namespace dds = dao::core::dds;

ToolSpec Spec(std::string name, std::string description, Json properties, const std::vector<std::string> &required)
{
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    schema["required"] = required;
    return ToolSpec{std::move(name), std::move(description), std::move(schema), Thread::Listener, false};
}

const Json kNormalProperty = {{"type", "boolean"},
                              {"description", "Whether it is a normal map, x, y, z in the PNG's red, green and "
                                              "blue. Default: its name ends in n, as the game's do (cn_bdy_ogra_0n)."}};

const Json kTextureProperties = {
    {"name",
     {{"type", "string"},
      {"description", "The texture, \"cn_bdy_ogra_0d.dds\" (the extension may be left off). A material (.mao, "
                      "`resource` shows one as text) names its textures; `resources` with type dds finds more."}}},
    {"source",
     {{"type", "string"},
      {"description", R"(Which copy, by its source ("core", "addin:..."). Default: the copy the game takes.)"}}},
    {"archive", {{"type", "string"}, {"description", "Which copy, by its archive's path."}}},
    {"to",
     {{"type", "string"},
      {"description", "The folder to write into, a full path; made if missing. Never inside the game's folder."}}},
};

// A texture's name with .dds, and its stem.
std::pair<std::string, std::string> Named(const Args &args)
{
    std::string name = Lower(args.String("name"));
    if (Extension(name).empty())
    {
        name += ".dds";
    }
    return {name, Stem(name)};
}

// The game names a texture by what it is for, its last letter: d diffuse, n
// normal, s specular, t tint, e emissive (cn_bdy_ogra_0d, _0n).
bool IsNormalMap(const std::string &stem)
{
    return stem.ends_with("n");
}

struct Read
{
    std::vector<std::byte> bytes;
    dds::Description description;
    Json copy;
};

Read ReadTexture(Ghilana &ghilana, const Args &args, const std::string &name, const std::string &tool)
{
    (void)ghilana.Indexed();
    const GameFiles &files = ghilana.Files();
    const auto copies = files.Select(tool, name, args.String("source", ""), args.String("archive", ""));
    const Copy &copy = GameFiles::Choose(copies);
    std::string why;
    auto bytes = files.Read(copy, &why);
    if (!bytes)
    {
        throw ToolError(tool + ": \"" + name + "\" cannot be read: " + why);
    }
    const auto description = dds::Describe(*bytes);
    if (!description || !description->decodes || description->shape != dds::Shape::Flat)
    {
        throw ToolError(tool + ": \"" + name +
                        "\" is no flat DDS picture DAOLib reads (DXT1, DXT3, DXT5, A4R4G4B4, or 24 or 32 bits a "
                        "pixel); `resource` says what it is");
    }
    return {std::move(*bytes), *description, Chosen(files, name, *files.Find(name), copy)};
}

// The game's smaller copies of a texture for its farther models (_0dl2,
// _0dl3: lib-agent, 2026-10-14), the ones it has.
std::vector<std::string> FarCopies(Ghilana &ghilana, const std::string &stem)
{
    std::vector<std::string> out;
    for (const char *suffix : {"l2", "l3"})
    {
        const std::string far = stem + suffix + ".dds";
        if (ghilana.Files().Find(far) != nullptr)
        {
            out.push_back(far);
        }
    }
    return out;
}

// ---------------------------------------------------------------- texture_export

Json Export(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "texture_export");
    const fs::path to = OutputFolder(ghilana, args, "texture_export");
    const auto [name, stem] = Named(args);
    const Read read = ReadTexture(ghilana, args, name, "texture_export");
    auto image = dds::Decode(read.bytes);
    if (!image)
    {
        throw ToolError("texture_export: \"" + name + "\" does not decode");
    }
    const bool normal = args.Bool("normal", IsNormalMap(stem));
    if (normal)
    {
        *image = dds::Normals(*image);
    }
    const auto png = EncodePng(image->width, image->height, 4, image->rgba);
    const fs::path path = to / fs::path(Wide(stem + ".png"));
    WriteFile(path, png, "texture_export");

    Json out = read.copy;
    out["png"] = PathText(path);
    out["format"] = read.description.format;
    out["width"] = image->width;
    out["height"] = image->height;
    out["pictures"] = read.description.pictures;
    out["normal_map"] = normal;
    if (normal)
    {
        out["normal_note"] = "a normal map: written with x, y, z in red, green and blue as an editor shows one (the "
                             "game keeps x in alpha); paint it as such, and `texture_import` puts it back";
    }
    if (const auto far = FarCopies(ghilana, stem); !far.empty())
    {
        out["far_copies"] = far;
    }
    out["next"] = "edit the PNG at the same or another size whose sides are powers of two, then `texture_import` it "
                  "with the same name";
    return out;
}

// ---------------------------------------------------------------- texture_import

// The format a texture is written back in: the original's, as the game keeps
// its maps (lib-agent, DAOLib #120), unless `format` says.
std::pair<dds::Format, std::string> FormatFor(const Args &args, const dds::Description &original, bool normal)
{
    const std::string asked =
        args.Has("format") ? args.Choice("format", {"dxt1", "dxt5", "dxt5_normal", "a4r4g4b4", "a8r8g8b8"}) : "";
    if (asked == "dxt1")
    {
        return {dds::Format::Dxt1, "DXT1"};
    }
    if (asked == "dxt5")
    {
        return {dds::Format::Dxt5, "DXT5"};
    }
    if (asked == "dxt5_normal")
    {
        return {dds::Format::Dxt5Normal, "DXT5, a normal map"};
    }
    if (asked == "a4r4g4b4")
    {
        return {dds::Format::A4R4G4B4, "A4R4G4B4"};
    }
    // A normal map in the game's layout whatever the original's file: its
    // shader reads x from alpha and y from green (lib-agent, DAOLib #120).
    if (asked.empty() && normal)
    {
        return {dds::Format::Dxt5Normal, "DXT5, a normal map"};
    }
    if (asked == "a8r8g8b8" || original.format == "B8G8R8A8" || original.format == "B8G8R8")
    {
        return {dds::Format::A8R8G8B8, "A8R8G8B8"};
    }
    if (original.format == "DXT1")
    {
        return {dds::Format::Dxt1, "DXT1"};
    }
    if (original.format == "A4R4G4B4")
    {
        return {dds::Format::A4R4G4B4, "A4R4G4B4"};
    }
    return {dds::Format::Dxt5, "DXT5"};
}

// Each pixel the average of four of the picture's.
dds::Image Halved(const dds::Image &image)
{
    dds::Image out;
    out.width = std::max<std::uint32_t>(1, image.width / 2);
    out.height = std::max<std::uint32_t>(1, image.height / 2);
    out.rgba.resize(std::size_t{out.width} * out.height * 4);
    for (std::uint32_t y = 0; y < out.height; ++y)
    {
        for (std::uint32_t x = 0; x < out.width; ++x)
        {
            for (std::size_t c = 0; c < 4; ++c)
            {
                unsigned sum = 0;
                for (std::uint32_t dy = 0; dy < 2; ++dy)
                {
                    for (std::uint32_t dx = 0; dx < 2; ++dx)
                    {
                        const std::uint32_t sx = std::min(image.width - 1, (x * 2) + dx);
                        const std::uint32_t sy = std::min(image.height - 1, (y * 2) + dy);
                        sum += image.rgba[(((std::size_t{sy} * image.width) + sx) * 4) + c];
                    }
                }
                out.rgba[(((std::size_t{y} * out.width) + x) * 4) + c] = static_cast<std::uint8_t>((sum + 2) / 4);
            }
        }
    }
    return out;
}

bool PowerOfTwo(std::uint32_t n)
{
    return n != 0 && (n & (n - 1)) == 0;
}

Json Import(Ghilana &ghilana, const Json &raw)
{
    const Args args(raw, "texture_import");
    const fs::path to = OutputFolder(ghilana, args, "texture_import");
    const auto [name, stem] = Named(args);
    const Read read = ReadTexture(ghilana, args, name, "texture_import");
    const fs::path from = fs::path(Wide(args.String("png")));
    std::ifstream in(from, std::ios::binary);
    if (!in)
    {
        throw ToolError("texture_import: cannot open " + PathText(from));
    }
    const std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    dds::Image image;
    try
    {
        PngImage png = DecodePng(std::as_bytes(std::span(chars)));
        image = {png.width, png.height, std::move(png.rgba)};
    }
    catch (const std::exception &error)
    {
        throw ToolError(std::string("texture_import: ") + error.what());
    }
    if (!PowerOfTwo(image.width) || !PowerOfTwo(image.height))
    {
        throw ToolError("texture_import: the picture is " + std::to_string(image.width) + " by " +
                        std::to_string(image.height) +
                        "; the game's textures have sides that are powers of two "
                        "(256, 512, 1024...): scale it to one");
    }
    const bool normal = args.Bool("normal", IsNormalMap(stem));
    const auto [format, formatName] = FormatFor(args, read.description, normal);
    const auto dds = dds::Encode(image, format);
    if (dds.empty())
    {
        throw ToolError("texture_import: DAOLib wrote no DDS of the picture");
    }
    const fs::path path = to / fs::path(Wide(name));
    WriteFile(path, dds, "texture_import");

    Json out = read.copy;
    Json written = Json::array({PathText(path)});
    out["format"] = formatName;
    out["original"] = {
        {"format", read.description.format}, {"width", read.description.width}, {"height", read.description.height}};
    out["width"] = image.width;
    out["height"] = image.height;
    out["normal_map"] = normal;
    // The farther models' smaller copies, at a half and a quarter of the
    // picture, so a model looks the same at every distance.
    if (args.Bool("far_copies", true))
    {
        dds::Image smaller = image;
        for (const std::string &far : FarCopies(ghilana, stem))
        {
            smaller = Halved(smaller);
            const fs::path farPath = to / fs::path(Wide(far));
            WriteFile(farPath, dds::Encode(smaller, format), "texture_import");
            written.push_back(PathText(farPath));
        }
    }
    out["written"] = std::move(written);
    out["into_the_game"] = "the game takes a loose file in " +
                           PathText(ghilana.Files().Where().documents / L"packages" / L"core" / L"override") +
                           " ahead of its archives (`conflicts` shows the copies); copy the files there, and take "
                           "them out to undo";
    return out;
}

} // namespace

void RegisterTextureTools(Bench &bench, Ghilana &ghilana)
{
    auto &tools = bench.Tools();
    Json exportProperties = kTextureProperties;
    exportProperties["normal"] = kNormalProperty;
    tools.Add(Spec("texture_export",
                   "Writes a texture (.dds) as a PNG of its largest picture into a folder of yours, for an image "
                   "editor: red, green, blue and alpha as the game draws them, and a normal map with x, y, z in red, "
                   "green and blue as an editor shows one. Answers the PNG's path, the original's format and size, "
                   "and the game's smaller copies for its farther models. `texture_import` puts it back.",
                   std::move(exportProperties), {"name", "to"}),
              [&ghilana](const Json &args) -> ToolResult { return Export(ghilana, args); });

    Json importProperties = kTextureProperties;
    importProperties["name"]["description"] = "The texture the PNG replaces, \"cn_bdy_ogra_0d.dds\": its format is "
                                              "the one written, and its name the file's.";
    importProperties["png"] = {{"type", "string"}, {"description", "The edited picture, a PNG's full path."}};
    importProperties["normal"] = kNormalProperty;
    importProperties["format"] = {
        {"type", "string"},
        {"enum", {"dxt1", "dxt5", "dxt5_normal", "a4r4g4b4", "a8r8g8b8"}},
        {"description", "The DDS format to write. Default: the original's (DXT3 becomes DXT5, 24 bits 32)."}};
    importProperties["far_copies"] = {{"type", "boolean"},
                                      {"description", "Also write the game's smaller copies for its farther models "
                                                      "(..._0dl2, _0dl3), from the picture halved. Default true."}};
    tools.Add(Spec("texture_import",
                   "Writes a PNG edited from `texture_export` back as the game's texture (.dds): in the original's "
                   "format with every halving of the picture down to a pixel, as the game's own are, and the game's "
                   "smaller copies for its farther models too. The picture's sides must be powers of two. Answers the "
                   "files written and where to put them for the game to take them. Writes nothing in the game's "
                   "folders.",
                   std::move(importProperties), {"name", "png", "to"}),
              [&ghilana](const Json &args) -> ToolResult { return Import(ghilana, args); });
}

} // namespace eluvian::ghilana::detail
