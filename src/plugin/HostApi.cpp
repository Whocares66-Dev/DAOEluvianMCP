#include "plugin/HostApi.h"

#include "api/EluvianAPI.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace eluvian::plugin
{

namespace
{

Bench *g_bench = nullptr;
std::mutex g_mutex;
std::set<std::string> g_builtIn; // the bench's own tools: no plugin replaces one
std::vector<std::string> g_registered;

// Where a plugin's handler answers: once, ok or error.
struct Sink
{
    EluvianReply reply{};
    std::optional<Json> result;
    std::optional<std::string> error;
    bool answered = false;
};

Sink &SinkOf(EluvianReply *reply)
{
    return *static_cast<Sink *>(reply->host);
}

void ReplyOk(EluvianReply *reply, const char *resultJson)
{
    Sink &sink = SinkOf(reply);
    if (std::exchange(sink.answered, true))
    {
        return; // answered already; the first answer stands
    }
    Json result = Json::parse(resultJson != nullptr ? resultJson : "null", nullptr, false);
    if (result.is_discarded())
    {
        sink.error = "the plugin answered with JSON that does not parse";
        return;
    }
    sink.result = std::move(result);
}

void ReplyError(EluvianReply *reply, const char *message)
{
    Sink &sink = SinkOf(reply);
    if (std::exchange(sink.answered, true))
    {
        return;
    }
    sink.error = message != nullptr ? message : "the plugin's tool failed and said nothing";
}

ToolHandler Wrap(std::string name, EluvianToolFn handler, void *context)
{
    return [name = std::move(name), handler, context](const Json &args) -> ToolResult {
        Sink sink;
        sink.reply = EluvianReply{&sink, &ReplyOk, &ReplyError};
        const std::string text = args.dump();
        try
        {
            handler(context, text.c_str(), &sink.reply);
        }
        catch (...)
        {
            // A C++ plugin's exception reached a C boundary; it stops here.
            throw ToolError(name + ": the plugin's handler threw");
        }
        if (sink.error)
        {
            throw ToolError(*sink.error);
        }
        if (!sink.result)
        {
            throw ToolError(name + ": the plugin's handler did not answer");
        }
        return std::move(*sink.result);
    };
}

bool NameIs(std::string_view name, std::string_view extra)
{
    return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [extra](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               extra.find(c) != std::string_view::npos;
    });
}

struct Descriptor
{
    std::string description;
    Json inputSchema;
};

std::optional<Descriptor> ReadDescriptor(const char *what, const char *descriptorJson)
{
    // Where the JSON breaks, said: turnbased's first registration had an
    // unescaped quote inside its description (2026-09-30), which a bare
    // "not JSON" left to be found by eye.
    Json descriptor;
    try
    {
        descriptor = Json::parse(descriptorJson != nullptr ? descriptorJson : "");
    }
    catch (const Json::parse_error &e)
    {
        spdlog::error("{}: refused, its descriptor does not parse as JSON (byte {}; a quote inside a string must be "
                      "escaped): {}",
                      what, e.byte, e.what());
        return std::nullopt;
    }
    if (!descriptor.is_object() || !descriptor.contains("description") || !descriptor["description"].is_string())
    {
        spdlog::error("{}: refused, its descriptor has no \"description\" string", what);
        return std::nullopt;
    }
    Json schema = descriptor.value("inputSchema", Json{{"type", "object"}});
    if (!schema.is_object())
    {
        spdlog::error("{}: refused, its inputSchema is not an object", what);
        return std::nullopt;
    }
    return Descriptor{descriptor["description"].get<std::string>(), std::move(schema)};
}

int RegisterTool(const char *name, const char *descriptorJson, EluvianToolFn handler, void *context,
                 std::uint32_t flags) noexcept
{
    try
    {
        const std::string_view tool = name != nullptr ? name : "";
        if (g_bench == nullptr || handler == nullptr || !NameIs(tool, ""))
        {
            spdlog::error("a plugin's tool \"{}\" refused: a name is letters, digits, '_' and '-', up to 64, and "
                          "a handler is needed",
                          tool);
            return -1;
        }
        const std::lock_guard lock(g_mutex);
        if (g_builtIn.contains(std::string(tool)))
        {
            spdlog::error("a plugin's tool \"{}\" refused: the bench has one of that name", tool);
            return -1;
        }
        auto descriptor = ReadDescriptor(name, descriptorJson);
        if (!descriptor)
        {
            return -1;
        }
        const bool added = g_bench->Tools().Add(
            ToolSpec{std::string(tool), std::move(descriptor->description), std::move(descriptor->inputSchema),
                     (flags & ELUVIAN_ON_LISTENER) != 0 ? Thread::Listener : Thread::Frame,
                     (flags & ELUVIAN_READ_ONLY) != 0},
            Wrap(std::string(tool), handler, context));
        if (added)
        {
            g_registered.emplace_back(tool);
        }
        spdlog::info("a plugin {} tool {}", added ? "registered" : "replaced", tool);
        return added ? 1 : 0;
    }
    catch (...)
    {
        return -1;
    }
}

int DescribeEvent(const char *topic, const char *descriptorJson) noexcept
{
    try
    {
        const std::string_view name = topic != nullptr ? topic : "";
        if (g_bench == nullptr || name.empty() || name.starts_with("eluvian.") || !NameIs(name, ".") ||
            name.size() > 64)
        {
            spdlog::error("a plugin's event \"{}\" refused: a topic is letters, digits, '_', '-' and '.', under "
                          "the plugin's own prefix (eluvian.* is the bench's)",
                          name);
            return -1;
        }
        const Json descriptor = Json::parse(descriptorJson != nullptr ? descriptorJson : "", nullptr, false);
        if (descriptor.is_discarded() || !descriptor.is_object() || !descriptor.contains("description") ||
            !descriptor["description"].is_string())
        {
            spdlog::error("{}: refused, its descriptor is not JSON with a \"description\" string", name);
            return -1;
        }
        Json fields = descriptor.value("fields", Json::object());
        if (!fields.is_object())
        {
            spdlog::error("{}: refused, its \"fields\" is not an object of field names to what they hold", name);
            return -1;
        }
        // Who said so: the topic's prefix, which the plugin's own name is.
        const std::string from(name.substr(0, name.find('.')));
        const bool added = g_bench->Topics().Describe(
            TopicSpec{std::string(name), descriptor["description"].get<std::string>(), std::move(fields), from});
        return added ? 1 : 0;
    }
    catch (...)
    {
        return -1;
    }
}

void EmitEvent(const char *topic, const char *payloadJson) noexcept
{
    try
    {
        if (g_bench == nullptr || topic == nullptr || *topic == '\0')
        {
            return;
        }
        Json payload = Json::parse(payloadJson != nullptr ? payloadJson : "{}", nullptr, false);
        if (payload.is_discarded())
        {
            payload = Json{{"unparsed", payloadJson}};
        }
        g_bench->Events().Emit(topic, std::move(payload));
    }
    catch (...)
    {
        // An event lost is better than an exception in another plugin's
        // frame; logging may be what failed, so the last place left.
        std::fputs("eluvian: a plugin's event was lost\n", stderr);
    }
}

constexpr EluvianInterface kInterface{
    &RegisterTool,
    &EmitEvent,
    &DescribeEvent,
};

} // namespace

void BindHostApi(Bench &bench)
{
    const std::lock_guard lock(g_mutex);
    g_bench = &bench;
    for (auto &name : bench.Tools().Names())
    {
        g_builtIn.insert(std::move(name));
    }
}

std::vector<std::string> RegisteredByPlugins()
{
    const std::lock_guard lock(g_mutex);
    return g_registered;
}

} // namespace eluvian::plugin

// A plugin built against another layout gets nothing, and carries on
// without Eluvian, as where it is absent.
extern "C" __declspec(dllexport) const EluvianInterface *Eluvian_GetInterface(std::uint32_t version)
{
    return version == ELUVIAN_INTERFACE_VERSION ? &eluvian::plugin::kInterface : nullptr;
}
