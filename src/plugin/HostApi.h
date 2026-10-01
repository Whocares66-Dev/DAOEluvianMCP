// The host side of src/api/EluvianAPI.h: what other plugins call to add
// tools, inspect kinds and events to the bench, exported as
// Eluvian_GetInterface.
#pragma once

#include "core/Bench.h"

#include <string>
#include <vector>

namespace eluvian::plugin
{

// Gives the interface its bench. Before any plugin can ask for it: at load.
void BindHostApi(Bench &bench);

// The names plugins registered, for `health`.
std::vector<std::string> RegisteredByPlugins();

} // namespace eluvian::plugin
