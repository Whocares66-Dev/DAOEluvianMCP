// SPDX-License-Identifier: MIT
//
// Eluvian's interface for other DAOLib plugins: register tools an agent can
// call, and emit and describe events it can wait for. This header
// alone is MIT, so a plugin under any licence may include it; Eluvian itself
// is GPLv3. Plain C across the boundary: JSON as strings, handlers as
// function pointers with a context, nothing with a destructor.
//
// Finding Eluvian: the loader loads plugins sorted by name, so eluvian.dll
// may load after yours. Look it up on your first frame (dao::OnFrame), not
// in DAOLib_Load:
//
//   if (const EluvianInterface *eluvian = EluvianFind()) {
//       eluvian->RegisterTool("mymod_reset", R"({"description": "Put mymod back as it starts."})",
//                             &EndTurn, &state, 0);
//       eluvian->EmitEvent("mymod.thing_happened", R"({"id": "0x82", "detail": "what it was"})");
//   }
//
// A handler is called on the game's frame thread (the scripts' thread, as
// dao::OnFrame's callbacks are) unless registered ELUVIAN_ON_LISTENER. It
// answers exactly once, through the reply: ok with the result as JSON, or
// error with a sentence the agent reads (what was wrong, what to do).
//
// Names: a tool's is what the agent calls it by, and only letters, digits,
// '_' and '-' reach a model (prefix yours: "mymod_reset"). An event topic
// may use dots ("mymod.thing_happened"); prefix it too.
//
// A tool is one concept: its own arguments, those it needs required in its
// schema. A plugin's state is a read-only tool of its own ("mymod_state",
// ELUVIAN_READ_ONLY), not a kind of another's (#110).
//
// Nothing is released, so nothing is kept for an older build: a plugin is
// built against the header of the Eluvian it runs with, and the two are
// deployed together. ELUVIAN_INTERFACE_VERSION changes with the
// interface's layout, so a mismatched pair refuse each other instead of a
// plugin calling through the wrong table.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // A C header, included by plugins in C and C++ alike: its typedefs
    // stay typedefs, `using` being C++ only (#77).
    // NOLINTBEGIN(modernize-use-using)

    typedef struct EluvianReply EluvianReply;
    struct EluvianReply
    {
        void *host; // Eluvian's own
        void (*ok)(EluvianReply *reply, const char *resultJson);
        void (*error)(EluvianReply *reply, const char *message);
    };

    typedef void (*EluvianToolFn)(void *context, const char *argsJson, EluvianReply *reply);

    // Flags for RegisterTool.
    enum
    {
        // Call the handler on the HTTP listener's thread, not the frame's: for
        // what touches no game state and must answer while frames stop.
        ELUVIAN_ON_LISTENER = 1,
        // MCP's readOnlyHint: the tool changes nothing in the game.
        ELUVIAN_READ_ONLY = 2,
    };

    typedef struct EluvianInterface
    {
        // descriptorJson: {"description": "...", "inputSchema": {...}}; the
        // schema may be left out. Returns 1 if added, 0 if it replaced one of
        // the same name, -1 if refused (the reason is in eluvian.log).
        int (*RegisterTool)(const char *name, const char *descriptorJson, EluvianToolFn handler, void *context,
                            uint32_t flags);
        // Publishes an event: payloadJson an object, its creatures as ids
        // ("0x7d9"). Any thread.
        void (*EmitEvent)(const char *topic, const char *payloadJson);
        // Says what an event topic means, so an agent can list the topics
        // there are (`events describe=true`) before waiting for one.
        // descriptorJson: {"description": "...", "fields": {"id": "the
        // creature", ...}}. Returns 1 if new, 0 if it replaced, -1 if refused.
        int (*DescribeEvent)(const char *topic, const char *descriptorJson);
    } EluvianInterface;

    typedef const EluvianInterface *(*EluvianGetInterfaceFn)(uint32_t version);

    // NOLINTEND(modernize-use-using)

#define ELUVIAN_GET_INTERFACE "Eluvian_GetInterface"

// The interface's layout: Eluvian_GetInterface answers only a plugin built
// against the same: 2 when #110 dropped inspect kinds and tool extensions, 3
// when it dropped the size and build kept for older builds.
#define ELUVIAN_INTERFACE_VERSION 3

#ifdef __cplusplus
}
#endif

#if defined(_WIN32) && defined(__cplusplus)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

// Eluvian's interface if eluvian.dll is loaded, else null: Eluvian is a
// development bench, absent from a player's install, and a plugin carries on
// without it.
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
