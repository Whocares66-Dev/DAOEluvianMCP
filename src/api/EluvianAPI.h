// SPDX-License-Identifier: MIT
//
// The interface a DAOLib plugin (your mod) uses to work with Eluvian. Through it you can:
//   - register tools that an agent can call, and
//   - publish events that an agent can wait for, and describe what they mean.
//
// This header is MIT licensed so that any plugin, whatever its own license, may include it.
// The rest of Eluvian is GPLv3.
//
// Everything crossing between your plugin and Eluvian is plain C: JSON is passed as strings,
// handlers are function pointers with a context pointer you choose, and nothing needs freeing.
//
// Finding Eluvian
// ---------------
// The loader loads plugins in order of their names, so eluvian.dll may not be loaded yet when
// your DAOLib_Load runs. Call EluvianFind on your first frame (in a dao::OnFrame callback)
// instead. It returns null when Eluvian is not installed, which is normal for players: your
// plugin should simply carry on without it.
//
//   static void ResetTool(void *context, const char *argsJson, EluvianReply *reply) {
//       static_cast<MyState *>(context)->Reset();
//       reply->ok(reply, "{}");
//   }
//
//   // On your first frame:
//   if (const EluvianInterface *eluvian = EluvianFind()) {
//       eluvian->RegisterTool("mymod_reset", R"({"description": "Reset mymod's state."})",
//                             &ResetTool, &state, 0);
//       eluvian->EmitEvent("mymod.thing_happened", R"({"id": "0x82", "detail": "what happened"})");
//   }
//
// Writing a handler
// -----------------
// A tool's handler runs on the game's frame thread (the same thread as dao::OnFrame callbacks
// and the game's scripts), unless you register it with ELUVIAN_ON_LISTENER. It must answer
// exactly once, through the reply it is given: call reply->ok with the result as JSON, or
// reply->error with a sentence for the agent saying what went wrong and what to do about it.
//
// Naming
// ------
// A tool's name is what the agent calls it by. Use only letters, digits, '_' and '-', and
// start it with your mod's name: "mymod_reset". An event topic may also use dots, and should
// start with your mod's name too: "mymod.thing_happened".
//
// Designing tools
// ---------------
// Make each tool do one thing, with its own arguments; mark the arguments it cannot do without
// as required in its input schema. To let an agent read your plugin's state, register a
// separate read-only tool for it, such as "mymod_state" with ELUVIAN_READ_ONLY.
//
// Versions
// --------
// Build your plugin against the same version of this header as the Eluvian it runs with, and
// deploy the two together. ELUVIAN_INTERFACE_VERSION changes whenever the interface's layout
// does. If the versions differ, EluvianFind returns null and your plugin carries on without
// Eluvian, rather than calling into a table it does not understand. An Eluvian serves the one
// version it was built with and none older: a release's notes say when the version changed,
// and a plugin is then built again.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Plugins may include this header from C as well as C++, so it uses typedef rather than
    // `using`, which is C++ only.
    // NOLINTBEGIN(modernize-use-using)

    // How a handler answers a call. Call exactly one of ok or error, exactly once, passing the
    // reply itself as the first argument.
    typedef struct EluvianReply EluvianReply;
    struct EluvianReply
    {
        void *host; // used by Eluvian; leave it alone
        // Succeed, with the result as a JSON string.
        void (*ok)(EluvianReply *reply, const char *resultJson);
        // Fail, with a sentence for the agent: what went wrong and what to do about it.
        void (*error)(EluvianReply *reply, const char *message);
    };

    // A tool's handler. context is the pointer you gave RegisterTool; argsJson is the agent's
    // arguments as a JSON object; reply is how you answer.
    typedef void (*EluvianToolFn)(void *context, const char *argsJson, EluvianReply *reply);

    // Flags for RegisterTool. Combine them with |, or pass 0 for none.
    enum
    {
        // Run the handler on Eluvian's HTTP thread instead of the game's frame thread. Use this
        // only for tools that never touch the game's state and must answer even when the game
        // stops running frames.
        ELUVIAN_ON_LISTENER = 1,
        // The tool changes nothing in the game. Agents see this as MCP's readOnlyHint.
        ELUVIAN_READ_ONLY = 2,
    };

    // What EluvianFind returns: the functions your plugin calls.
    typedef struct EluvianInterface
    {
        // Adds a tool the agent can call. descriptorJson is
        // {"description": "...", "inputSchema": {...}}; inputSchema is optional.
        // Returns 1 if the tool was added, 0 if it replaced an existing tool of the same name,
        // or -1 if it was refused (eluvian.log says why).
        int (*RegisterTool)(const char *name, const char *descriptorJson, EluvianToolFn handler, void *context,
                            uint32_t flags);
        // Publishes an event. payloadJson must be a JSON object; give creatures as ids such as
        // "0x7d9". Safe to call from any thread.
        void (*EmitEvent)(const char *topic, const char *payloadJson);
        // Explains what an event topic means, so an agent can list the available topics
        // (`events describe=true`) before waiting for one. descriptorJson is
        // {"description": "...", "fields": {"id": "the creature", ...}}.
        // Returns 1 if the description is new, 0 if it replaced an earlier one, or -1 if it
        // was refused (eluvian.log says why).
        int (*DescribeEvent)(const char *topic, const char *descriptorJson);
    } EluvianInterface;

    // The type of Eluvian's exported Eluvian_GetInterface function. EluvianFind calls it for you.
    typedef const EluvianInterface *(*EluvianGetInterfaceFn)(uint32_t version);

    // NOLINTEND(modernize-use-using)

#define ELUVIAN_GET_INTERFACE "Eluvian_GetInterface"

// The version of the interface's layout. It changes whenever the layout does, and
// Eluvian_GetInterface returns null to a plugin built against a different version.
#define ELUVIAN_INTERFACE_VERSION 3

#ifdef __cplusplus
}
#endif

#if defined(_WIN32) && defined(__cplusplus)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

// Returns Eluvian's interface, or null if eluvian.dll is not loaded or was built against a
// different interface version. Eluvian is a development tool that players will not have
// installed, so a plugin should carry on normally without it. Call this on your first frame,
// not in DAOLib_Load (see the top of this file).
inline const EluvianInterface *EluvianFind()
{
    const HMODULE eluvian = GetModuleHandleW(L"eluvian.dll");
    if (eluvian == nullptr)
    {
        return nullptr;
    }
    const auto get = reinterpret_cast<EluvianGetInterfaceFn>(GetProcAddress(eluvian, ELUVIAN_GET_INTERFACE));
    return get != nullptr ? get(ELUVIAN_INTERFACE_VERSION) : nullptr;
}
#endif
