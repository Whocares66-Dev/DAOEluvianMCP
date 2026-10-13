// ghilana.exe: the MCP server of the tools that need no game (#128,
// dev/GHILANA.md). An MCP client starts it and talks to it over stdio, a
// JSON-RPC message a line; it logs to stderr and to ghilana.log in its
// cache folder, never to stdout, which is the protocol's.
//
//   ghilana.exe [--game DIR] [--documents DIR] [--instance NAME] [--language CODE] [--cache DIR]
//               [--campaign UID|none] [--textures high|medium]
//   ghilana.exe --call TOOL [JSON]     one call, its answer on stdout (as the bridge's --call)
//   ghilana.exe --describe             every tool as tools/list gives it, for the docs
#include "core/Stdio.h"
#include "ghilana/Folders.h"
#include "ghilana/Ghilana.h"
#include "ghilana/Text.h"

#include <dao/core/Instance.h>

#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace
{

using eluvian::Json;
namespace gh = eluvian::ghilana;
namespace fs = std::filesystem;

struct Options
{
    std::optional<fs::path> game;
    std::optional<fs::path> documents;
    std::optional<fs::path> cache;
    std::string instance;
    std::string language = "en-us";
    std::string campaign = "Single Player";
    std::string textures = "high";
    std::optional<std::string> call;
    std::string callArgs = "{}";
    bool describe = false;
};

constexpr const char *kUsage =
    "ghilana " EL_VERSION_STRING " -- Dragon Age: Origins' files over MCP (stdio)\n"
    "  ghilana.exe [--game DIR] [--documents DIR] [--instance NAME] [--language CODE] [--cache DIR]\n"
    "              [--campaign UID|none] [--textures high|medium]\n"
    "    --campaign: the campaign the game is taken to have loaded, as AddIns.xml names one\n"
    "                (\"Single Player\", the default; DAO_PRC_EP_1 for Awakening; none for the main menu)\n"
    "  ghilana.exe --call TOOL [JSON]   one call, its answer on stdout\n"
    "  ghilana.exe --describe           every tool, as JSON\n";

// An option that takes a value, given it; false for no such option.
bool Take(const std::string &arg, const std::wstring &value, Options &options)
{
    if (arg == "--game")
    {
        options.game = fs::path(value);
    }
    else if (arg == "--documents")
    {
        options.documents = fs::path(value);
    }
    else if (arg == "--cache")
    {
        options.cache = fs::path(value);
    }
    else if (arg == "--instance")
    {
        options.instance = gh::Utf8(value);
    }
    else if (arg == "--language")
    {
        options.language = gh::Lower(gh::Utf8(value));
    }
    else if (arg == "--campaign")
    {
        options.campaign = gh::Lower(gh::Utf8(value)) == "none" ? std::string() : gh::Utf8(value);
    }
    else if (arg == "--textures")
    {
        options.textures = gh::Lower(gh::Utf8(value));
    }
    else if (arg == "--call")
    {
        options.call = gh::Utf8(value);
    }
    else
    {
        return false;
    }
    return true;
}

std::optional<Options> Parse(int argc, wchar_t **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = gh::Utf8(argv[i]);
        if (arg == "--describe")
        {
            options.describe = true;
            continue;
        }
        if (arg == "--help" || arg == "-h")
        {
            std::fputs(kUsage, stdout);
            return std::nullopt;
        }
        if (i + 1 >= argc || !Take(arg, argv[i + 1], options))
        {
            std::fprintf(stderr, "ghilana: %s %s\n%s", arg.c_str(),
                         i + 1 >= argc ? "needs a value, or is unknown" : "is unknown", kUsage);
            return std::nullopt;
        }
        ++i;
        // --call's arguments, if the next word is no option.
        if (arg == "--call" && i + 1 < argc && !std::wstring_view(argv[i + 1]).starts_with(L"--"))
        {
            options.callArgs = gh::Utf8(argv[++i]);
        }
    }
    return options;
}

// Lines to stderr and to the log file, each with its time.
class Log
{
  public:
    void Open(const fs::path &file)
    {
        const std::scoped_lock guard(lock_);
        file_.open(file, std::ios::app);
    }
    void Line(const std::string &text)
    {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
        localtime_s(&local, &now);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
        const std::scoped_lock guard(lock_);
        std::fprintf(stderr, "[%s] %s\n", stamp, text.c_str());
        if (file_)
        {
            file_ << '[' << stamp << "] " << text << '\n';
            file_.flush();
        }
    }

  private:
    std::mutex lock_;
    std::ofstream file_;
};

// The folders served: as the options name them, or as found.
std::optional<gh::Paths> Where(const Options &options, const std::vector<gh::Install> &installs,
                               const std::optional<fs::path> &local)
{
    gh::Paths paths;
    paths.language = options.language;
    paths.campaign = options.campaign;
    paths.textures = options.textures;
    if (paths.textures != "high" && paths.textures != "medium")
    {
        std::fprintf(stderr, "ghilana: --textures is high or medium\n");
        return std::nullopt;
    }
    if (options.game)
    {
        paths.game = *options.game;
    }
    else if (!installs.empty())
    {
        paths.game = installs.front().folder;
    }
    if (!options.instance.empty())
    {
        const auto name = dao::core::InstanceOf(options.instance);
        if (!name || !local)
        {
            std::fprintf(stderr, "ghilana: --instance takes 1 to 32 letters, digits, - and _\n");
            return std::nullopt;
        }
        paths.instance = *name;
    }
    if (options.documents)
    {
        paths.documents = *options.documents;
    }
    else if (!paths.instance.empty())
    {
        paths.documents = dao::core::InstanceDocuments(*local, paths.instance) / L"BioWare" / L"Dragon Age";
    }
    else if (const auto documents = gh::GameDocuments())
    {
        paths.documents = *documents;
    }
    return paths;
}

fs::path CacheFolder(const Options &options, const std::optional<fs::path> &local)
{
    if (options.cache)
    {
        return *options.cache;
    }
    if (local)
    {
        return *local / L"Eluvian" / L"ghilana";
    }
    return fs::temp_directory_path() / L"ghilana";
}

// One --call: its answer on stdout, or its refusal on stderr.
int Call(eluvian::Bench &bench, const Options &options)
{
    Json args;
    try
    {
        args = Json::parse(options.callArgs);
    }
    catch (const Json::parse_error &e)
    {
        std::fprintf(stderr, "ghilana: --call's arguments are not JSON: %s\n", e.what());
        return 2;
    }
    try
    {
        const auto result = bench.Invoke(*options.call, args);
        std::fputs(result.data.dump(1, ' ', false, Json::error_handler_t::replace).c_str(), stdout);
        std::fputs("\n", stdout);
        for (const auto &item : result.content)
        {
            std::fprintf(stderr, "(and content of type %s, not printed)\n", item.value("type", "?").c_str());
        }
        return 0;
    }
    catch (const eluvian::ToolError &e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    catch (const eluvian::UnknownTool &e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
}

int Run(const Options &options)
{
    Log log;
    try
    {
        const auto local = gh::LocalAppData();
        const auto installs = gh::FindInstalls();
        const auto paths = Where(options, installs, local);
        if (!paths)
        {
            return 2;
        }
        const fs::path cache = CacheFolder(options, local);
        std::error_code error;
        fs::create_directories(cache, error);
        // Each call logged when serving; a --call's answer or refusal is its
        // output already.
        const bool serving = !options.describe && !options.call;
        if (serving)
        {
            log.Open(cache / L"ghilana.log");
        }

        gh::Ghilana ghilana(*paths, installs, cache, EL_VERSION_STRING);
        eluvian::BenchOptions benchOptions;
        benchOptions.onCall = [&log, serving](const eluvian::CallRecord &call) {
            if (serving)
            {
                const std::string refused = call.error.empty() ? "" : " refused: " + std::string(call.error);
                log.Line(std::string(call.tool) + " " + std::to_string(static_cast<long long>(call.ms)) + " ms" +
                         refused);
            }
        };
        eluvian::Bench bench(std::move(benchOptions));
        gh::RegisterTools(bench, ghilana);

        if (options.describe)
        {
            const Json out = {{"version", EL_VERSION_STRING}, {"tools", bench.Tools().Describe()}};
            std::fputs(out.dump(1, ' ', false, Json::error_handler_t::replace).c_str(), stdout);
            std::fputs("\n", stdout);
            return 0;
        }
        if (options.call)
        {
            return Call(bench, options);
        }
        log.Line("ghilana " EL_VERSION_STRING " serving " + gh::PathText(paths->game) + " (documents " +
                 gh::PathText(paths->documents) + ", campaign " +
                 (paths->campaign.empty() ? std::string("none") : paths->campaign) + ", language " + paths->language +
                 ", textures " + paths->textures + ")");
        eluvian::McpEndpoint mcp(bench, eluvian::ServerInfo{"ghilana", EL_VERSION_STRING, gh::Instructions()});
        eluvian::ServeStdio(mcp, std::cin, std::cout);
        log.Line("stdin closed; ghilana ends");
        return 0;
    }
    catch (const std::exception &e)
    {
        log.Line(std::string("ghilana failed: ") + e.what());
        return 1;
    }
}

} // namespace

// The C runtime's entry point for wide arguments: it must have external
// linkage, which misc-use-internal-linkage does not know of wmain.
int wmain(int argc, wchar_t **argv) // NOLINT(misc-use-internal-linkage): above
{
    // The protocol's bytes as they are: no CR put before each LF.
    (void)_setmode(_fileno(stdout), _O_BINARY);
    (void)_setmode(_fileno(stdin), _O_BINARY);
    const auto options = Parse(argc, argv);
    if (!options)
    {
        return 2;
    }
    return Run(*options);
}
