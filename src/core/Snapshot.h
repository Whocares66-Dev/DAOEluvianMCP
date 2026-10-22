// `snapshot` and `snapshot_diff`: one frame's state in a schema another engine
// can write too, and two such compared (#167). DAOParagonEngine checks every
// behaviour it ports against the running game, the same save and inputs in
// both, by the state they reach ("read before port", the user's rule for it,
// 2026-10-22, relayed by paragon-agent); that took a call of each reading tool
// and a diff by hand. src/game's `snapshot` reads the game; what is decided
// over a snapshot, its creatures' keys and the comparison, is here, tested.
//
// A snapshot names its creatures without ids, which differ between engines:
// `tag|template#k`, k counting the creatures of that tag and template in the
// order they were made (the object id's order in the game; the area's list,
// then the spawns, in an engine that builds the area from its file).
#pragma once

#include "core/Bench.h"

#include <string>
#include <utility>
#include <vector>

namespace eluvian
{

// What the schema calls itself, so a file written by another engine says
// which it follows.
inline constexpr const char *kSnapshotSchema = "eluvian/snapshot 1";

// Each creature's key, given (tag, template) in the order they were made.
std::vector<std::string> CreatureKeys(const std::vector<std::pair<std::string, std::string>> &inOrderMade);

// How far two snapshots may differ and be the same: metres, degrees, points
// of health or stamina, and seconds (the game time, and an effect's time
// left).
struct SnapshotTolerance
{
    double position = 0.01;
    double facing = 1.0;
    double health = 0.0;
    double stamina = 0.0;
    double time = 0.05;
};

// `tolerance` as a call gives it, over the defaults; throws ToolError for a
// field there is not or a negative one.
SnapshotTolerance ToleranceFrom(const Json &given);

// A snapshot as a call gives it: the JSON itself, or a file's path (written
// by an engine, or a snapshot kept). Throws ToolError naming `which` for a
// file that cannot be read or is not a snapshot.
Json SnapshotFrom(const Json &given, const std::string &which);

// What differs between `a` and `b`: `same`, `count`, `differences` (each a
// JSON pointer `path`, `a`, `b`, and `by` for a measure), the game times'
// `skew` where it is past the tolerance, and `notCompared`, the fields only
// one side has. At most `most` differences are listed.
Json SnapshotDiff(const Json &a, const Json &b, const SnapshotTolerance &tolerance, std::size_t most = 200);

// snapshot_diff, a listener tool: it needs no game.
void AddSnapshotDiffTool(Bench &bench);

} // namespace eluvian
