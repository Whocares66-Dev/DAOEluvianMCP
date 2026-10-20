cmake_minimum_required(VERSION 3.21)

project(DAOEluvianMCP VERSION 0.1.0 LANGUAGES CXX)

# C++20 for what the build fetches too: Catch2 compiled as its own default
# has no printer for std::string_view, which the tests then lack at link
# (core-cov, where Catch2 is fetched rather than vcpkg's).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(
    EL_BUILD_PLUGIN
    "Build eluvian.dll and its server (needs DAOLib and the game's 32-bit architecture)"
    ON
)
option(EL_BUILD_TESTS "Build the unit tests (no game required)" ON)
# ghilana.exe, the server of the tools that need no game (#128,
# dev/GHILANA.md): built with the plugin (so the debug gate builds, tests
# and lints it, 32-bit) and alone in the ghilana presets, 64-bit, which is
# what is released. The core presets build neither.
option(
    EL_BUILD_GHILANA
    "Build ghilana.exe and its tests (needs DAOLib's core)"
    ${EL_BUILD_PLUGIN}
)
option(EL_SANITIZE "Build with AddressSanitizer (for the tests)" OFF)
option(EL_ANALYZE "MSVC static analysis (/analyze) on our own targets" OFF)
option(
    EL_COVERAGE
    "Instrument for test coverage (clang-cl only; the core-cov preset)"
    OFF
)

# Dragon Age: Origins is a 32-bit x86 executable, so the plugin the loader
# loads into it must be too. CMakePresets.json selects the x86 toolset and
# the x86-windows-static-md vcpkg triplet; this catches a configure from a
# 64-bit shell with a sentence rather than a DLL the game cannot load. The
# core alone may be built either way (core-cov is 64-bit).
if(EL_BUILD_PLUGIN AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(
        FATAL_ERROR
        "eluvian.dll must be built for 32-bit x86 (the game's architecture); "
        "configure with a preset from CMakePresets.json, through tools/build.ps1"
    )
endif()

# Our own targets: max warnings, warnings fail the build, conformance, and
# third-party headers (DAOLib's and nlohmann's, through <angle brackets>)
# kept quiet. The flags DAOLib and DAOTurnBased build with.
function(el_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /Zc:preprocessor)
            target_compile_options(
                ${target}
                PRIVATE /external:anglebrackets /external:W0
            )
            # C4702, unreachable code, is the optimiser's word after
            # inlining: a test's function that always throws made
            # FrameQueue's set_value after it unreachable in the release
            # build only (#76, 2026-10-05). The debug gate keeps it.
            target_compile_options(
                ${target}
                PRIVATE $<$<NOT:$<CONFIG:Debug>>:/wd4702>
            )
        endif()
        if(EL_ANALYZE AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(
                ${target}
                PRIVATE /analyze /analyze:external-
            )
        endif()
    else()
        target_compile_options(
            ${target}
            PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
        )
    endif()
    target_compile_features(${target} PRIVATE cxx_std_20)
endfunction()

# AddressSanitizer covers every translation unit or none, so it is global
# and set before any target. Only the tests run under it; a sanitized DLL in
# the game would need the ASan runtime beside the exe. vcpkg's libraries are
# built without the STL's container annotations, so they are off (DAOLib's
# CMakeLists has the LNK2038 this avoids).
if(EL_SANITIZE)
    add_compile_options(/fsanitize=address /Oy-)
    add_compile_definitions(
        _DISABLE_STRING_ANNOTATION
        _DISABLE_VECTOR_ANNOTATION
    )
    add_link_options(/INCREMENTAL:NO)
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

# Test coverage, clang's source-based kind, as DAOTurnBased measures it:
# every region of el_core gets a counter, el_tests runs once, and llvm-cov
# reports (the `coverage` target, tests/CMakeLists.txt). clang-cl only; LLVM
# ships the profile runtime for x64 alone, so the core-cov preset is 64-bit.
if(EL_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(
            FATAL_ERROR
            "EL_COVERAGE needs clang-cl (the core-cov preset); the compiler is ${CMAKE_CXX_COMPILER_ID}"
        )
    endif()
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
        OUTPUT_VARIABLE _clang_resource_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    link_directories("${_clang_resource_dir}/lib/windows")
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    get_filename_component(_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(EL_LLVM_PROFDATA llvm-profdata HINTS "${_llvm_bin}")
    find_program(EL_LLVM_COV llvm-cov HINTS "${_llvm_bin}")
endif()

# nlohmann-json from vcpkg in the x86 presets; core-cov builds with no vcpkg
# at all, so there it is fetched, as Catch2 is (tests/CMakeLists.txt).
find_package(nlohmann_json 3 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found through find_package; fetching it.")
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL
            https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---------------------------------------------------------------------------
# el_core -- the bench without the game: MCP's JSON-RPC, the REST routes, the
# tool registry, the event bus, the queue to the frame thread, arguments,
# logs read from a cursor.
#
# It never includes <Windows.h>, never links DAOLib and never sees a dao::
# type, which is what makes it testable without the game (CLAUDE.md).
# ---------------------------------------------------------------------------
add_library(
    el_core
    STATIC
    src/core/Args.cpp
    src/core/Base64.cpp
    src/core/Bench.cpp
    src/core/Conversation.cpp
    src/core/CharGen.cpp
    src/core/Craft.cpp
    src/core/EventBus.cpp
    src/core/EventLog.cpp
    src/core/Expect.cpp
    src/core/FrameQueue.cpp
    src/core/Http.cpp
    src/core/LevelUp.cpp
    src/core/LogTail.cpp
    src/core/Loot.cpp
    src/core/Mcp.cpp
    src/core/Origin.cpp
    src/core/Party.cpp
    src/core/GfxList.cpp
    src/core/Gift.cpp
    src/core/Placeables.cpp
    src/core/Ready.cpp
    src/core/Requirements.cpp
    src/core/Rest.cpp
    src/core/Scenario.cpp
    src/core/Sight.cpp
    src/core/Stdio.cpp
    src/core/ToolRegistry.cpp
    src/core/Travel.cpp
    src/core/Until.cpp
    src/core/Warp.cpp
)
el_apply_warnings(el_core)
target_include_directories(el_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(el_core PUBLIC nlohmann_json::nlohmann_json)

if(EL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# The core presets stop here: no DAOLib, no server, no plugin, and no format
# or lint targets, which read the plugin's compile database (run them on
# debug).
if(NOT EL_BUILD_PLUGIN AND NOT EL_BUILD_GHILANA)
    return()
endif()

if(EL_BUILD_PLUGIN)
    # -----------------------------------------------------------------------
    # el_server -- the socket loop under core/Http.h: 127.0.0.1 only, a
    # thread per connection. It needs Winsock but not the game, so its tests
    # run over loopback.
    # -----------------------------------------------------------------------
    add_library(el_server STATIC src/server/HttpServer.cpp)
    el_apply_warnings(el_server)
    target_link_libraries(el_server PUBLIC el_core ws2_32)

    if(EL_BUILD_TESTS)
        add_subdirectory(tests/server)
    endif()
endif()

# ---------------------------------------------------------------------------
# DAOLib, built with us the way DAOTurnBased builds it: a source tree added
# as a subdirectory, which gives DAOLib::daolib and add_daolib_plugin. Its
# dependencies (detours, spdlog) come through our vcpkg.json, since a
# manifest build reads only the top-level manifest.
#
# The tree is extern/DAOLib, a git submodule, once there is one; until then,
# and whenever DAOLib is being changed alongside this, the sibling checkout
# ..\DAOLib. DAOLIB_DIR names another.
# ---------------------------------------------------------------------------
set(DAOLIB_DIR
    ""
    CACHE PATH
    "DAOLib's source tree (default extern/DAOLib, else ../DAOLib)"
)
if(DAOLIB_DIR)
    set(EL_DAOLIB "${DAOLIB_DIR}")
elseif(EXISTS "${PROJECT_SOURCE_DIR}/extern/DAOLib/CMakeLists.txt")
    set(EL_DAOLIB "${PROJECT_SOURCE_DIR}/extern/DAOLib")
else()
    set(EL_DAOLIB "${PROJECT_SOURCE_DIR}/../DAOLib")
endif()
if(NOT EXISTS "${EL_DAOLIB}/cmake/DAOLib.cmake")
    message(
        FATAL_ERROR
        "No DAOLib with add_daolib_plugin at ${EL_DAOLIB}. Add the submodule "
        "(git submodule add <DAOLib's url> extern/DAOLib) or set DAOLIB_DIR."
    )
endif()
message(STATUS "DAOLib: ${EL_DAOLIB}")
add_subdirectory(
    "${EL_DAOLIB}"
    "${CMAKE_BINARY_DIR}/extern/DAOLib"
    EXCLUDE_FROM_ALL
)

if(EL_BUILD_GHILANA)
    include(cmake/Ghilana.cmake)
endif()

# The ghilana presets stop here: no plugin, and the format and lint targets
# are the debug preset's, which builds ghilana too.
if(NOT EL_BUILD_PLUGIN)
    return()
endif()

# The format and lint targets, DAOLib's own module run over this tree: it
# reads the top-level source directory, our .clang-format and .clang-tidy,
# and tools/check-powershell.ps1.
include("${EL_DAOLIB}/cmake/Quality.cmake")

# ---------------------------------------------------------------------------
# eluvian.dll -- the plugin. src/game is every call into the engine, through
# DAOLib; it is verified by playing.
# ---------------------------------------------------------------------------
# el_game: the game's tools and their registration, built once for the
# plugin and for eluvian_describe, which prints them for the docs without the
# game (#109).
add_library(
    el_game
    OBJECT
    src/game/Camera.cpp
    src/game/Capture.cpp
    src/game/Board.cpp
    src/game/Character.cpp
    src/game/Convert.cpp
    src/game/Dialogue.cpp
    src/game/Game.cpp
    src/game/GameTools.cpp
    src/game/Gui.cpp
    src/game/Health.cpp
    src/game/Input.cpp
    src/game/Inspect.cpp
    src/game/Order.cpp
    src/game/Resources.cpp
    src/game/Screen.cpp
    src/game/Screenshot.cpp
    src/game/Script.cpp
    src/game/Shared.cpp
    src/game/Stage.cpp
    src/game/Tactics.cpp
    src/game/Store.cpp
    src/game/Warp.cpp
    src/game/WorldMap.cpp
    src/plugin/Tools.cpp
)
el_apply_warnings(el_game)
# windowscodecs: the screenshot's JPEG (game/Capture.cpp).
target_link_libraries(el_game PUBLIC el_server DAOLib::daolib windowscodecs)
target_compile_definitions(
    el_game
    PUBLIC EL_VERSION_STRING="${PROJECT_VERSION}"
)

# The bridge goes beside the DLL, so the plugin's folder is the whole of what
# is installed, in the build tree, in the game and in a release's zip (#115):
# an agent's client starts it from there (tools/eluvian_bridge.py's head).
add_daolib_plugin(
    eluvian
    SOURCES src/plugin/HostApi.cpp src/plugin/Plugin.cpp
    FILES tools/eluvian_bridge.py
)
el_apply_warnings(eluvian)
target_link_libraries(eluvian PRIVATE el_game)
# What the game plays is what is released (#76, #115). RelWithDebInfo links
# incrementally unless told otherwise, which pads the DLL for the next link
# and keeps every function nothing calls: 5.6 MB and an 80 MB PDB on
# 2026-10-10. /DEBUG also turns /OPT:REF and /OPT:ICF off, so they are put
# back, as DAOLib's dao_release_symbols does for its Release builds.
if(MSVC)
    target_link_options(
        eluvian
        PRIVATE "$<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF>"
    )
    # The linker writes into a PDB that is already there and leaves in it
    # what earlier links wrote: after one relink the PDB was 81 MB where a
    # new one is 40, and still held the path of the temporary files of a
    # link made before tools\build.ps1 kept those in the build tree, which
    # is under the user's name (2026-10-10). So each link starts its own.
    # A debug link is incremental and needs the one it has.
    if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        add_custom_command(
            TARGET eluvian
            PRE_LINK
            COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_PDB_FILE:eluvian>
            VERBATIM
        )
    endif()
endif()

# eluvian_describe: every tool and topic Eluvian serves, as JSON, without
# the game: what the docs site's reference is built from (#109).
add_executable(eluvian_describe src/plugin/Describe.cpp)
el_apply_warnings(eluvian_describe)
target_link_libraries(eluvian_describe PRIVATE el_game)
# The site's reference is what eluvian_describe prints, kept in
# docs/_data/eluvian.json for the Linux build that cannot run it: this fails
# when the two differ, or a page will not render, saying how to refresh it.
if(EL_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        # And ghilana's, from ghilana --describe (docs/_data/ghilana.json),
        # which this preset builds too (cmake/Ghilana.cmake).
        add_test(
            NAME docs.reference_current
            COMMAND
                "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tools/build_docs.py" --check --exe
                $<TARGET_FILE:eluvian_describe> --ghilana-exe
                $<TARGET_FILE:ghilana>
        )
    endif()
endif()

# `cmake --build --preset debug` builds the loader too, so build\<preset>\
# is a game folder's worth of files: dinput8.dll (DAOLib's target `loader`;
# version.dll and `version` before DAOLib #93), daolib\plugins\<name>\ (a
# folder a plugin, DAOLib #26) and
# daolib\addresses\*.bin (the address library's files, one per build of the
# game, without which the loader loads nothing; built with whatever links
# daolib, DAOLib 8ecb9ac on).
add_custom_target(game_folder ALL DEPENDS loader eluvian)
