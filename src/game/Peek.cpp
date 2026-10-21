// `peek`: a field of a live object, or of the exe's data, by a chain of
// offsets (#161; core/Peek.h has the chain and the decoding). The game's
// memory is read through DAOLib's guarded copy (dao/SafeRead.h), so an
// address that is no longer mapped is an answer, and each pointer on the way
// is named by its run-time type (dao/TypeName.h), which checks every read too.
// It runs on the frame thread, where the object found by its id is not freed
// under the read. Nothing is written.
#include "core/Peek.h"
#include "game/Shared.h"

#include <dao/Module.h>
#include <dao/SafeRead.h>
#include <dao/TypeName.h>

namespace eluvian::game
{

namespace
{

PeekMemory GameMemory()
{
    return {[](std::uint32_t address, void *out, std::size_t size) { return dao::TryCopy(out, address, size); },
            [](std::uint32_t address) {
                // NOLINTNEXTLINE(performance-no-int-to-ptr): an address the agent gave, read through checked reads
                return dao::TypeName(reinterpret_cast<const void *>(static_cast<std::uintptr_t>(address)));
            }};
}

// Where the chain starts, and what the answer says of it.
std::uint32_t Base(const Args &args, Json &out)
{
    const int given = (args.Has("object") ? 1 : 0) + (args.Has("address") ? 1 : 0) + (args.Has("address_id") ? 1 : 0);
    if (given != 1)
    {
        throw ToolError("peek: give one of `object` (an id, \"0x7d9\"), `address` (\"0x00b2b290\") or `address_id` "
                        "(an ID of the address library)");
    }
    if (args.Has("address_id"))
    {
        const auto id = static_cast<std::uint32_t>(args.Integer("address_id", 0, 1, UINT32_MAX));
        const std::uintptr_t address = dao::Module::Address(static_cast<dao::AddressId>(id));
        if (address == 0)
        {
            throw ToolError("peek: the address library has no ID " + std::to_string(id) + " for this build");
        }
        out["address_id"] = id;
        return static_cast<std::uint32_t>(address);
    }
    if (args.Has("address"))
    {
        const auto offsets = PeekOffsets(Json::array({args.Raw()["address"]}));
        return static_cast<std::uint32_t>(offsets.front());
    }
    const ObjectId id = args.Id("object", "an object, as creatures or placeables list them");
    const std::string side = args.Choice("side", {"game", "client"}, "game");
    auto *app = dao::CClientExoApp::Get();
    const void *object = nullptr;
    if (app != nullptr)
    {
        object = side == "game" ? static_cast<const void *>(app->GetGameObjectById(id))
                                : static_cast<const void *>(app->GetObjectById(id));
    }
    if (object == nullptr)
    {
        throw ToolError("peek: no " + side + " object " + FormatId(id) + " in the area");
    }
    out["object"] = FormatId(id);
    out["side"] = side;
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(object));
}

ToolResult PeekTool(const Json &raw)
{
    const Args args(raw, "peek");
    Json out = Json::object();
    const std::uint32_t base = Base(args, out);
    const std::string type = args.String("type", "u32");
    const auto count = static_cast<std::size_t>(args.Integer("count", type == "bytes" ? 16 : 1, 1, 1024));
    const Json offsets = args.Has("offsets") ? args.Raw()["offsets"] : Json();
    Json read = Peek(GameMemory(), base, PeekOffsets(offsets), type, count);
    out.update(read);
    return out;
}

} // namespace

void AddPeek(Bench &bench)
{
    bench.Tools().Add(
        Read(
            "peek",
            "Reads a field of a live object, or of the game's own data, by a chain of offsets: from the start, "
            "every offset but the last is a pointer read and followed, and the value is read at the last, as "
            "`type`. Use it to check a reading of the exe against the running game: a field at +0x64 of the "
            "object a creature holds at +0x7B0 is `{\"object\": \"0x7d9\", \"offsets\": [\"0x7B0\", \"0x64\"], "
            "\"type\": \"f32\"}`. Each pointer on the way is named by its class (RTTI). A read that fails is "
            "answered with `readable: false` and where, never a crash. Nothing is written.",
            Json{{"object",
                  {{"type", "string"},
                   {"description", "An object's id, \"0x7d9\", as creatures, placeables or party list them."}}},
                 {"side",
                  {{"type", "string"},
                   {"enum", Json::array({"game", "client"})},
                   {"description", "With `object`: the game's object (CGameObject, the scripts' side), or the "
                                   "client's (CCObject, what is drawn). Default game."}}},
                 {"address",
                  {{"type", "string"},
                   {"description", "In place of `object`: an address in the running game, \"0x00b2b290\"."}}},
                 {"address_id",
                  {{"type", "integer"},
                   {"description", "In place of `object`: an ID of the address library (DAOLib's "
                                   "tools/address_ids.py names them), its address in the running build."}}},
                 {"offsets",
                  {{"type", "array"},
                   {"items", {{"type", Json::array({"integer", "string"})}}},
                   {"description", "The chain: numbers, or hex as text (\"0x7B0\", \"-0x10\"). None reads at the "
                                   "start itself."}}},
                 {"type",
                  {{"type", "string"},
                   {"enum", Json::array({"f32", "i32", "u32", "u16", "u8", "ptr", "vec3", "quat", "wstring", "bytes"})},
                   {"description", "What the value is: a float, an integer, a pointer (with its class), three or four "
                                   "floats, the engine's std::wstring, or raw bytes as hex. Default u32."}}},
                 {"count",
                  {{"type", "integer"},
                   {"minimum", 1},
                   {"maximum", 1024},
                   {"description", "How many values one after another (bytes: how many bytes, default 16). "
                                   "Default 1."}}}},
            {}),
        &PeekTool);
}

} // namespace eluvian::game
