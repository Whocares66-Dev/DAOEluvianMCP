// What is decided over a snapshot: its creatures' keys, and two compared.
// See Snapshot.h.
#include "core/Snapshot.h"

#include "core/Args.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace eluvian
{

namespace
{

// The creature fields compared by a measure; every other field both sides
// have is compared as it is, and the names (`key`, the engine's own `id`, the
// text `name`, which a language changes) not at all.
const std::set<std::string, std::less<>> kMeasured = {"position", "facingDegrees", "health", "stamina", "effects"};
const std::set<std::string, std::less<>> kNotCompared = {"key", "id", "name"};

// A JSON pointer's token: `~` and `/` escaped, as RFC 6901 has them.
std::string Token(std::string_view text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '~')
        {
            out += "~0";
        }
        else if (c == '/')
        {
            out += "~1";
        }
        else
        {
            out += c;
        }
    }
    return out;
}

// Two values the same, numbers within a hair (one engine's float written as
// another's double).
bool Same(const Json &a, const Json &b)
{
    if (a.is_number() && b.is_number())
    {
        return std::abs(a.get<double>() - b.get<double>()) <= 1e-6;
    }
    if (a.is_array() && b.is_array())
    {
        return a.size() == b.size() && std::ranges::equal(a, b, Same);
    }
    if (a.is_object() && b.is_object())
    {
        return a.size() == b.size() && std::ranges::all_of(a.items(), [&b](const auto &item) {
                   const auto other = b.find(item.key());
                   return other != b.end() && Same(item.value(), *other);
               });
    }
    return a == b;
}

// A number at `key` of `object`, if it is one.
std::optional<double> NumberAt(const Json &object, std::string_view key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number())
    {
        return std::nullopt;
    }
    return found->get<double>();
}

class Differ
{
  public:
    Differ(const SnapshotTolerance &tolerance, std::size_t most) : tolerance_(tolerance), most_(most)
    {
    }

    void Top(const Json &a, const Json &b)
    {
        Time(a, b);
        for (const char *field : {"area", "mode", "paused", "controlled"})
        {
            Exact(a, b, field, std::string("/") + field);
        }
        Party(a, b);
        Creatures(a, b);
        Plots(a, b);
        Vars(a, b);
    }

    Json Answer() &&
    {
        Json answer = {{"same", count_ == 0}, {"count", count_}};
        if (skew_)
        {
            answer["skew"] = *skew_;
            answer["note"] = "the game times differ past the tolerance: what moves with time (positions, health, "
                             "effects' time left) differs from the skew alone";
        }
        answer["differences"] = std::move(differences_);
        if (count_ > most_)
        {
            answer["truncated"] = true;
        }
        answer["compared"] = compared_;
        if (!notCompared_.empty())
        {
            answer["notCompared"] = Json(notCompared_);
        }
        return answer;
    }

  private:
    void Note(const std::string &path, const Json &a, const Json &b, std::optional<double> by = std::nullopt)
    {
        ++count_;
        if (differences_.size() >= most_)
        {
            return;
        }
        Json entry = {{"path", path}, {"a", a}, {"b", b}};
        if (by)
        {
            entry["by"] = *by;
        }
        differences_.push_back(std::move(entry));
    }

    // A field compared as it is where both sides have it; noted where one
    // has it.
    void Exact(const Json &a, const Json &b, std::string_view field, const std::string &path)
    {
        const bool inA = a.contains(field);
        const bool inB = b.contains(field);
        if (inA != inB)
        {
            notCompared_.insert(path);
            return;
        }
        if (inA && !Same(a.at(field), b.at(field)))
        {
            Note(path, a.at(field), b.at(field));
        }
    }

    // A number past its tolerance.
    void Measure(const Json &a, const Json &b, std::string_view field, const std::string &path, double tolerance)
    {
        const auto x = NumberAt(a, field);
        const auto y = NumberAt(b, field);
        if (!x || !y)
        {
            if (x.has_value() != y.has_value())
            {
                notCompared_.insert(path);
            }
            return;
        }
        if (const double by = std::abs(*x - *y); by > tolerance + 1e-9)
        {
            Note(path, *x, *y, by);
        }
    }

    void Time(const Json &a, const Json &b)
    {
        const auto x = NumberAt(a, "gameTime");
        const auto y = NumberAt(b, "gameTime");
        if (!x || !y)
        {
            notCompared_.insert("/gameTime");
            return;
        }
        if (const double by = std::abs(*x - *y); by > tolerance_.time + 1e-9)
        {
            skew_ = *y - *x;
            Note("/gameTime", *x, *y, by);
        }
    }

    // The party as a set: the engines may list it in another order.
    void Party(const Json &a, const Json &b)
    {
        if (!a.contains("party") || !b.contains("party"))
        {
            if (a.contains("party") != b.contains("party"))
            {
                notCompared_.insert("/party");
            }
            return;
        }
        auto sorted = [](const Json &list) {
            std::vector<std::string> keys;
            for (const auto &key : list)
            {
                keys.push_back(key.is_string() ? key.get<std::string>() : key.dump());
            }
            std::ranges::sort(keys);
            return keys;
        };
        if (sorted(a.at("party")) != sorted(b.at("party")))
        {
            Note("/party", a.at("party"), b.at("party"));
        }
    }

    void Creatures(const Json &a, const Json &b)
    {
        const Json none = Json::object();
        const Json &x = a.contains("creatures") ? a.at("creatures") : none;
        const Json &y = b.contains("creatures") ? b.at("creatures") : none;
        std::set<std::string> keys;
        for (const auto &item : x.items())
        {
            keys.insert(item.key());
        }
        for (const auto &item : y.items())
        {
            keys.insert(item.key());
        }
        for (const auto &key : keys)
        {
            const std::string path = "/creatures/" + Token(key);
            const bool inA = x.contains(key);
            const bool inB = y.contains(key);
            if (!inA || !inB)
            {
                Note(path, inA ? Json(true) : Json(nullptr), inB ? Json(true) : Json(nullptr));
                continue;
            }
            compared_["creatures"] = compared_.value("creatures", 0) + 1;
            Creature(x.at(key), y.at(key), path);
        }
    }

    void Creature(const Json &a, const Json &b, const std::string &path)
    {
        Position(a, b, path);
        Facing(a, b, path);
        for (const auto &[field, tolerance] :
             {std::pair{"health", tolerance_.health}, std::pair{"stamina", tolerance_.stamina}})
        {
            const bool inA = a.contains(field);
            if (inA != b.contains(field))
            {
                notCompared_.insert(std::string("/creatures/*/") + field);
            }
            else if (inA)
            {
                for (const char *part : {"current", "max"})
                {
                    Measure(a.at(field), b.at(field), part, path + "/" + field + "/" + part, tolerance);
                }
            }
        }
        Effects(a, b, path);
        for (const auto &item : a.items())
        {
            if (kMeasured.contains(item.key()) || kNotCompared.contains(item.key()))
            {
                continue;
            }
            if (!b.contains(item.key()))
            {
                notCompared_.insert("/creatures/*/" + Token(item.key()));
                continue;
            }
            if (!Same(item.value(), b.at(item.key())))
            {
                Note(path + "/" + Token(item.key()), item.value(), b.at(item.key()));
            }
        }
        for (const auto &item : b.items())
        {
            if (!a.contains(item.key()) && !kNotCompared.contains(item.key()))
            {
                notCompared_.insert("/creatures/*/" + Token(item.key()));
            }
        }
    }

    void Position(const Json &a, const Json &b, const std::string &path)
    {
        const auto pa = a.find("position");
        const auto pb = b.find("position");
        if (pa == a.end() || pb == b.end())
        {
            if ((pa == a.end()) != (pb == b.end()))
            {
                notCompared_.insert("/creatures/*/position");
            }
            return;
        }
        double squared = 0;
        for (const char *axis : {"x", "y", "z"})
        {
            const double d = NumberAt(*pa, axis).value_or(0) - NumberAt(*pb, axis).value_or(0);
            squared += d * d;
        }
        if (const double by = std::sqrt(squared); by > tolerance_.position + 1e-9)
        {
            Note(path + "/position", *pa, *pb, by);
        }
    }

    // Degrees the short way round: 179 and -179 are 2 apart.
    void Facing(const Json &a, const Json &b, const std::string &path)
    {
        const auto x = NumberAt(a, "facingDegrees");
        const auto y = NumberAt(b, "facingDegrees");
        if (!x || !y)
        {
            if (x.has_value() != y.has_value())
            {
                notCompared_.insert("/creatures/*/facingDegrees");
            }
            return;
        }
        double by = std::fmod(std::abs(*x - *y), 360.0);
        by = std::min(by, 360.0 - by);
        if (by > tolerance_.facing + 1e-9)
        {
            Note(path + "/facingDegrees", *x, *y, by);
        }
    }

    // Effects matched by type and ability, the nth of a kind with the nth;
    // their time left within the time's tolerance.
    void Effects(const Json &a, const Json &b, const std::string &path)
    {
        const bool inA = a.contains("effects");
        if (inA != b.contains("effects"))
        {
            notCompared_.insert("/creatures/*/effects");
            return;
        }
        if (!inA)
        {
            return;
        }
        auto byKind = [](const Json &effects) {
            std::map<std::string, std::vector<Json>> kinds;
            for (const auto &effect : effects)
            {
                kinds[Json{effect.value("type", Json(nullptr)), effect.value("ability", Json(nullptr))}.dump()]
                    .push_back(effect);
            }
            return kinds;
        };
        auto x = byKind(a.at("effects"));
        auto y = byKind(b.at("effects"));
        for (const auto &[kind, effects] : x)
        {
            const auto &others = y[kind];
            for (std::size_t i = 0; i < std::max(effects.size(), others.size()); ++i)
            {
                const std::string at = std::format("{}/effects/{}/{}", path, Token(kind), i);
                if (i >= effects.size() || i >= others.size())
                {
                    Note(at, i < effects.size() ? effects[i] : Json(nullptr),
                         i < others.size() ? others[i] : Json(nullptr));
                    continue;
                }
                Measure(effects[i], others[i], "remaining", at + "/remaining", tolerance_.time);
            }
        }
        for (const auto &[kind, others] : y)
        {
            if (!x.contains(kind))
            {
                for (std::size_t i = 0; i < others.size(); ++i)
                {
                    Note(std::format("{}/effects/{}/{}", path, Token(kind), i), nullptr, others[i]);
                }
            }
        }
    }

    // Plots as {plot: {flag: set}}; a flag only one side gives is a
    // difference, since a flag not given is not known to be clear.
    void Plots(const Json &a, const Json &b)
    {
        const bool inA = a.contains("plots");
        if (inA != b.contains("plots"))
        {
            notCompared_.insert("/plots");
            return;
        }
        if (!inA)
        {
            return;
        }
        Json merged = a.at("plots");
        merged.update(b.at("plots"));
        for (const auto &plot : merged.items())
        {
            const Json &x = a.at("plots").value(plot.key(), Json::object());
            const Json &y = b.at("plots").value(plot.key(), Json::object());
            Json flags = x;
            flags.update(y);
            for (const auto &flag : flags.items())
            {
                compared_["flags"] = compared_.value("flags", 0) + 1;
                const Json va = x.value(flag.key(), Json(nullptr));
                const Json vb = y.value(flag.key(), Json(nullptr));
                if (!Same(va, vb))
                {
                    Note("/plots/" + Token(plot.key()) + "/" + Token(flag.key()), va, vb);
                }
            }
        }
    }

    // Variables as [{object, name, type, value}], matched by object and name.
    void Vars(const Json &a, const Json &b)
    {
        const bool inA = a.contains("vars");
        if (inA != b.contains("vars"))
        {
            notCompared_.insert("/vars");
            return;
        }
        if (!inA)
        {
            return;
        }
        auto byName = [](const Json &vars) {
            std::map<std::string, Json> named;
            for (const auto &var : vars)
            {
                named[var.value("object", std::string()) + "/" + var.value("name", std::string())] =
                    var.value("value", Json(nullptr));
            }
            return named;
        };
        const auto x = byName(a.at("vars"));
        const auto y = byName(b.at("vars"));
        std::map<std::string, Json> all = x;
        all.insert(y.begin(), y.end());
        for (const auto &[name, value] : all)
        {
            compared_["vars"] = compared_.value("vars", 0) + 1;
            const auto ia = x.find(name);
            const auto ib = y.find(name);
            const Json va = ia == x.end() ? Json(nullptr) : ia->second;
            const Json vb = ib == y.end() ? Json(nullptr) : ib->second;
            if (!Same(va, vb))
            {
                Note("/vars/" + Token(name), va, vb);
            }
        }
    }

    SnapshotTolerance tolerance_;
    std::size_t most_;
    std::size_t count_ = 0;
    std::optional<double> skew_;
    Json differences_ = Json::array();
    Json compared_ = Json::object();
    std::set<std::string> notCompared_;
};

} // namespace

std::vector<std::string> CreatureKeys(const std::vector<std::pair<std::string, std::string>> &inOrderMade)
{
    std::map<std::pair<std::string, std::string>, int> seen;
    std::vector<std::string> keys;
    keys.reserve(inOrderMade.size());
    for (const auto &kind : inOrderMade)
    {
        keys.push_back(std::format("{}|{}#{}", kind.first, kind.second, seen[kind]++));
    }
    return keys;
}

SnapshotTolerance ToleranceFrom(const Json &given)
{
    SnapshotTolerance tolerance;
    if (given.is_null())
    {
        return tolerance;
    }
    if (!given.is_object())
    {
        throw ToolError(R"(snapshot_diff: `tolerance` is an object: {"position": 0.01, "time": 0.05, ...})");
    }
    const std::map<std::string, double *, std::less<>> fields = {{"position", &tolerance.position},
                                                                 {"facing", &tolerance.facing},
                                                                 {"health", &tolerance.health},
                                                                 {"stamina", &tolerance.stamina},
                                                                 {"time", &tolerance.time}};
    for (const auto &item : given.items())
    {
        const auto field = fields.find(item.key());
        if (field == fields.end())
        {
            throw ToolError(std::format("snapshot_diff: `tolerance` has no `{}`; it takes position, facing, "
                                        "health, stamina and time",
                                        item.key()));
        }
        if (!item.value().is_number() || item.value().get<double>() < 0)
        {
            throw ToolError(std::format("snapshot_diff: `tolerance.{}` is a number, 0 or more", item.key()));
        }
        *field->second = item.value().get<double>();
    }
    return tolerance;
}

Json SnapshotFrom(const Json &given, const std::string &which)
{
    if (given.is_object())
    {
        return given;
    }
    if (!given.is_string())
    {
        throw ToolError(
            std::format("snapshot_diff: `{}` is a snapshot, as snapshot answers it, or a file's path", which));
    }
    const auto path = given.get<std::string>();
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw ToolError(std::format("snapshot_diff: `{}`: no file can be read at {}", which, path));
    }
    std::stringstream text;
    text << file.rdbuf();
    Json read = Json::parse(text.str(), nullptr, false);
    if (read.is_discarded() || !read.is_object())
    {
        throw ToolError(std::format("snapshot_diff: `{}`: {} is not a snapshot's JSON object", which, path));
    }
    return read;
}

Json SnapshotDiff(const Json &a, const Json &b, const SnapshotTolerance &tolerance, std::size_t most)
{
    Differ differ(tolerance, most);
    differ.Top(a, b);
    return std::move(differ).Answer();
}

void AddSnapshotDiffTool(Bench &bench)
{
    const Json snapshot = {{"description", "A snapshot as `snapshot` answers it (or another engine writes it, "
                                           "the same schema), or the path of a file holding one."}};
    const Json schema = {
        {"type", "object"},
        {"properties",
         {{"a", snapshot},
          {"b", snapshot},
          {"tolerance",
           {{"type", "object"},
            {"description", "How far a value may differ and still be the same: position (metres, default 0.01), "
                            "facing (degrees, 1), health and stamina (points, 0), time (seconds, 0.05: the game "
                            "time and an effect's time left)."}}},
          {"most",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 5000},
            {"description", "The most differences to list. Default 200; `count` has them all."}}}}},
        {"required", {"a", "b"}}};
    bench.Tools().Add(
        ToolSpec{"snapshot_diff",
                 "Compares two snapshots and says what differs: the same save and inputs run in two engines, or "
                 "the game at two moments. Pass `a` and `b`, each a snapshot or a file holding one. Creatures are "
                 "matched by their key (tag|template#k), not by id.\n\n"
                 "The answer has `same`, `count` and `differences`, each with a JSON pointer `path`, the values `a` "
                 "and `b`, and `by` for a distance or an amount. Where the game times differ past the tolerance, "
                 "`skew` is b's minus a's: line the two up before reading the rest. `notCompared` lists fields only "
                 "one side gave. Needs no game.",
                 schema, Thread::Listener, true},
        [](const Json &raw) -> ToolResult {
            const Args args(raw, "snapshot_diff");
            if (!args.Has("a") || !args.Has("b"))
            {
                throw ToolError("snapshot_diff: pass `a` and `b`, the two snapshots (or their files) to compare");
            }
            const Json a = SnapshotFrom(args.Raw().at("a"), "a");
            const Json b = SnapshotFrom(args.Raw().at("b"), "b");
            const auto tolerance = ToleranceFrom(args.Raw().value("tolerance", Json(nullptr)));
            const auto most = static_cast<std::size_t>(args.Integer("most", 200, 1, 5000));
            return SnapshotDiff(a, b, tolerance, most);
        });
}

} // namespace eluvian
