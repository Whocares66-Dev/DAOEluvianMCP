// A wait on a condition, not an event: a tool called again and again until
// an expectation on its answer holds, or the time runs out. A driver's
// "stand until the party's health is back", "until the creature is dead",
// "until the mode is off" each polled inspect from outside (turn-based-
// agent's survey of its driver, 2026-10-06, #85). The tool `wait_until`,
// and scenario's `until` step, both run PollUntil.
#pragma once

#include "core/Bench.h"

#include <chrono>

namespace eluvian
{

// `until`: {"tool", "args", "pointer", and an expectation (core/Expect.h),
// "intervalMs"}. Calls the tool every `intervalMs` until the expectation
// holds, or `budget` is spent. A call that fails (a frame not taken while an
// area loads, say) counts as not yet, its error kept. Answers `held`, the
// last `value`, `tries`, `waitedMs`, and what the last failed (`unmet`,
// `lastError`). Throws ToolError for a malformed `until`, UnknownTool for a
// tool there is not.
Json PollUntil(Bench &bench, const Json &until, std::chrono::milliseconds budget);

void AddWaitUntilTool(Bench &bench);

} // namespace eluvian
