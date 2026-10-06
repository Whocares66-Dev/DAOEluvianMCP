// An expectation over a value in a tool's answer, as scenario's `expect`
// step and wait_until (core/Until.h) both speak it: `pointer` (a JSON
// pointer into the answer) and one of `equals`, `notEquals`, `exists`,
// `atLeast`, `atMost`. Moved out of core/Scenario.cpp when wait_until came
// to need it (#85).
#pragma once

#include "core/Tool.h"

#include <optional>
#include <string>
#include <string_view>

namespace eluvian::expect
{

// The value at `pointer` ("/given", "/creatures/0/id") in an answer;
// nothing if there is none there. A pointer that is not one is refused,
// the error led by `tool`.
std::optional<Json> At(const Json &answer, const std::string &pointer, std::string_view tool = "scenario");

// What of the expectation the value fails, or empty where it holds.
std::string Check(const Json &expect, const std::optional<Json> &actual);

} // namespace eluvian::expect
