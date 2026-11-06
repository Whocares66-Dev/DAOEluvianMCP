// `move_trace`: one creature's move updates as the engine runs them, over a
// span, through DAOLib's dao::movement (DAOLib #325), and the walker's
// state that `creature` adds to its answer (#194).
//
// Asked for a creature's movement compared with another engine's
// (paragon-agent-2, DAOParagonEngine #429): the path alone (`creature`'s
// `path`) did not say why a walker stood, waited or gave up. The move
// update says it: its answer, how long the creature has stood, and what the
// creature-in-the-way pass listed and did. Since #195 (DAOLib #327) also
// the answer to the update's own path request, the held stretch's points,
// and what lets the standing count run: a brought wolf stood 0.6 to 1 s at
// standingMs 0, result 0 and motion run (paragon-agent-2).
//
// The watch is armed on the frame thread, where DAOLib places its detours
// at the first watch, and only for the call's span: with no watch alive an
// update costs DAOLib an atomic test. The updates come on the frame thread;
// the call waits on the listener's, so frames go on while it waits, and a
// span that sees no frame (an area loading) is a time run out with no
// samples, not a stall.
#include "game/GameTools.h"
#include "game/Shared.h"

#include <dao/Movement.h>
#include <dao/World.h>

#include <condition_variable>
#include <memory>
#include <mutex>

namespace eluvian::game
{

namespace
{

namespace movement = dao::movement;

const char *MotionName(std::int32_t motion)
{
    switch (motion)
    {
    case movement::motion::kIdle:
        return "idle";
    case movement::motion::kWalk:
        return "walk";
    case movement::motion::kRun:
        return "run";
    case movement::motion::kStopping:
        return "stopping";
    case movement::motion::kTurning:
        return "turning";
    default:
        return "unknown";
    }
}

const char *RecordName(std::int32_t state)
{
    switch (state)
    {
    case movement::record::kClear:
        return "clear";
    case movement::record::kRepath:
        return "repath";
    case movement::record::kSettling:
        return "settling";
    case movement::record::kWaiting:
        return "waiting";
    default:
        return "unknown";
    }
}

// The move update's answer in words, as DAOLib's MoveUpdate::result has it.
const char *ResultName(std::int32_t result)
{
    switch (result)
    {
    case 1:
        return "arrived";
    case 0:
        return "going";
    case -4:
        return "stuck or no path";
    case -8:
        return "done while waiting";
    case -9:
        return "a door in the way";
    default:
        return result < 0 ? "failed" : "unknown";
    }
}

// The answer to the update's own path request in words, by DAOLib's
// dao::world::path codes (DAOLib #327). 2, 3 and 7 are walked as found is.
const char *PathAnswerName(std::int32_t answer)
{
    namespace path = dao::world::path;
    switch (answer)
    {
    case path::kFound:
    case 2:
    case 3:
    case 7:
        return "found";
    case path::kSearching:
        return "searching";
    case path::kNoPath:
        return "no path";
    case path::kActionRequired:
        return "a door in the way";
    case path::kNoWalkmesh:
        return "no walkmesh";
    default:
        return "unknown";
    }
}

Json RecordJson(const movement::Record &record)
{
    return {{"state", RecordName(record.state)}, {"code", record.state}, {"timerMs", record.timerMs}};
}

Json WalkerJson(const movement::Walker &walker)
{
    Json answer = {{"motion", MotionName(walker.motion)}};
    answer["record"] = walker.record ? RecordJson(*walker.record) : Json(nullptr);
    return answer;
}

Json InTheWayJson(const movement::InTheWay &pass)
{
    Json listed = Json::array();
    for (const ObjectId id : pass.listed)
    {
        listed.push_back(FormatId(id));
    }
    Json answer = {
        {"combat", pass.combat}, {"listed", std::move(listed)}, {"answer", pass.answer}, {"reacted", pass.reacted}};
    // The record around the reaction only where it ran: otherwise it is the
    // same before and after, and every walking frame repeated it.
    if (pass.reacted)
    {
        answer["before"] = RecordJson(pass.before);
        answer["after"] = RecordJson(pass.after);
    }
    return answer;
}

// What one call's watch and its wait share, under `lock`. Shared with the
// watch's callback, which may still be running on the frame thread when the
// call has answered.
struct Trace
{
    std::mutex lock;
    std::condition_variable changed;
    ObjectId creature = kInvalidObject;
    bool toEnd = true;
    std::size_t most = 0;
    std::chrono::steady_clock::time_point start;
    Json samples = Json::array();
    std::string ended; // why it ended before the span ran out; empty while it runs
};

Json SampleJson(const movement::MoveUpdate &update, std::uint64_t frame, double ms)
{
    Json sample = {{"frame", frame},
                   {"ms", ms},
                   {"subAction", update.subAction},
                   {"result", update.result},
                   {"says", ResultName(update.result)},
                   {"standingMs", update.standingMs},
                   {"walker", WalkerJson(update.walker)}};
    // The update's own path request, read as it answered, never asked by
    // us: a probe of ours left a stale path behind in the turn-based mod
    // (turn-based-agent, #195). Null where the update asked none.
    sample["path"] = update.pathAnswer
                         ? Json{{"answer", *update.pathAnswer}, {"says", PathAnswerName(*update.pathAnswer)}}
                         : Json(nullptr);
    // The held stretch: a walker asks its path a stretch at a time, so this
    // is the next point or two, not the route.
    sample["stretch"] = {{"points", update.waypoints}, {"at", update.waypoint}};
    // standingMs adds only while all of these let it (DAOLib #327); a walker
    // that stands with them off stands at 0.
    sample["counter"] = {{"counting", update.counting}, {"aiLevel", update.aiLevel}, {"updateMs", update.updateMs}};
    sample["inTheWay"] = update.inTheWay ? InTheWayJson(*update.inTheWay) : Json(nullptr);
    return sample;
}

ToolResult MoveTrace(Bench &bench, const Watch &watch, const Json &raw)
{
    const Args args(raw, "move_trace");
    const auto span = std::chrono::milliseconds(args.Integer("ms", 3000, 100, 30000));
    auto trace = std::make_shared<Trace>();
    trace->toEnd = args.Bool("toEnd", true);
    trace->most = static_cast<std::size_t>(args.Integer("most", 600, 1, 5000));
    const std::optional<ObjectId> asked = args.OptionalId("id", kCreatureHint);

    struct Armed
    {
        movement::Watch handle;
        Json before;
    };
    auto armed = bench.Frames().RunAndWait(
        [&watch, &asked, trace] {
            RequireWorld(watch, "move_trace");
            const ObjectId id = asked.value_or(api::GetMainControlled());
            const auto walker = id != kInvalidObject ? movement::WalkerOf(id) : std::nullopt;
            if (!walker)
            {
                throw ToolError(std::format("move_trace: {} is not a creature; creatures lists them",
                                            id == kInvalidObject ? "nothing" : FormatId(id)));
            }
            trace->creature = id;
            trace->start = std::chrono::steady_clock::now();
            const Watch *frames = &watch;
            auto handle = movement::OnMoveUpdate([trace, frames](const movement::MoveUpdate &update) {
                if (update.creature != trace->creature)
                {
                    return;
                }
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - trace->start).count();
                {
                    const std::lock_guard hold(trace->lock);
                    if (!trace->ended.empty())
                    {
                        return;
                    }
                    trace->samples.push_back(SampleJson(update, frames->Frames(), ms));
                    if (trace->toEnd && update.result != 0)
                    {
                        trace->ended = "move";
                    }
                    else if (trace->samples.size() >= trace->most)
                    {
                        trace->ended = "most";
                    }
                    else
                    {
                        return;
                    }
                }
                trace->changed.notify_all();
            });
            return std::make_shared<Armed>(Armed{std::move(handle), WalkerJson(*walker)});
        },
        std::chrono::milliseconds(5000));

    std::unique_lock hold(trace->lock);
    trace->changed.wait_for(hold, span, [&trace] { return !trace->ended.empty(); });
    if (trace->ended.empty())
    {
        trace->ended = "time";
    }
    hold.unlock();
    // Off the frame thread: DAOLib's Stop takes the watch's own lock, and a
    // callback running now still holds `trace`, which outlives it.
    armed->handle.Stop();

    hold.lock();
    Json answer = {{"id", FormatId(trace->creature)},
                   {"ended", trace->ended},
                   {"before", std::move(armed->before)},
                   {"count", trace->samples.size()}};
    if (trace->samples.empty())
    {
        answer["note"] = "no move update ran for it in the span: it was not walking. Order the walk first "
                         "(order), then call this at once; an attack's approach is a move too";
    }
    answer["samples"] = std::move(trace->samples);
    return answer;
}

} // namespace

Json WalkerNow(ObjectId id)
{
    const auto walker = movement::WalkerOf(id);
    return walker ? WalkerJson(*walker) : Json(nullptr);
}

void AddMovement(Bench &bench, const Watch &watch)
{
    bench.Tools().Add(
        Read("move_trace",
             "Records one creature's move updates as the engine runs them, one sample a frame while it walks, "
             "for a span: why a walker stands, waits, re-paths or gives up, which `creature`'s `path` does not "
             "say. Each sample has the `frame`, `ms` since the call, the running sub-action's type "
             "(`subAction`), the update's `result` (1 arrived, 0 going, negative a failure: -4 stuck or no "
             "path, -8 done while waiting, -9 a door in the way) and `says` it in words, `standingMs` (how long "
             "it has stood while it should walk; past 1000 in combat or 2000 out of it the move fails), "
             "`counter` (whether that count runs: it adds only while `counting` is true, `aiLevel` is over 1 "
             "and `updateMs` is under 300, so a walker standing with any of them off shows standingMs 0), "
             "`path` (the answer to the update's own path request, null where it asked none: `answer` 1 found, "
             "4 searching, which stands the walker that update, 5 no path, 6 a door in the way, 8 no walkmesh, "
             "and `says` it), `stretch` (the held stretch's `points` and the one walked to, `at`: a walker "
             "asks its path a stretch at a time, so this is not the whole route), the "
             "`walker` after the update (`motion`: idle, walk, run, stopping or turning; `record`: clear, "
             "repath, settling or waiting, with its timer) and `inTheWay` where the update ran that pass: the "
             "creatures `listed` in the way, the pass's `answer` (0 none blocks, 1 one does, 2 the walk stops "
             "for the controlled creature), whether the reaction ran (`reacted`) and the record `before` and "
             "`after` it; `combat` for the in-combat pass. Order the walk first (`order`), then call this at "
             "once: it waits `ms` (3000) and ends early when the move ends (`toEnd`) or at `most` samples. "
             "`ended` says which: move, most or time. A creature standing still is mostly walked around by "
             "the path search, so the pass lists someone mainly where walkers cross.",
             Json{{"id",
                   {{"type", "string"}, {"description", "The creature, by id. Default: the creature you control."}}},
                  {"ms",
                   {{"type", "integer"}, {"description", "Milliseconds to record for, 100 to 30000. Default 3000."}}},
                  {"toEnd",
                   {{"type", "boolean"},
                    {"description", "End at the first update that arrives or fails. Default true."}}},
                  {"most", {{"type", "integer"}, {"description", "Samples to keep at most, 1 to 5000. Default 600."}}}},
             {}, true, Thread::Listener),
        [&bench, &watch](const Json &raw) -> ToolResult { return MoveTrace(bench, watch, raw); });
}

} // namespace eluvian::game
