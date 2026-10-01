// `scenario`: a list of steps run in the bench, one call for what would be
// a dozen: call a tool, wait for an event, pause, check an answer. Returns a
// transcript, a line per step. devbench's scenario tool is the model
// (C:\modding\devbench, "Scripted tests").
//
//   {"steps": [
//     {"tool": "order", "args": {"action": "attack", "target": "0x7d9"}, "as": "order"},
//     {"waitFor": {"topic": "eluvian.command.ended"}, "timeoutMs": 10000, "as": "next"},
//     {"expect": {"from": "order", "pointer": "/given", "equals": true}}
//   ]}
//
// A wait starts from the cursor taken just before the latest tool step, so
// an event the tool caused before it answered is not missed; `since` gives
// another. A step that fails stops the run, unless `continueOnError`.
#pragma once

#include "core/Bench.h"

namespace eluvian
{

void AddScenarioTool(Bench &bench);

} // namespace eluvian
