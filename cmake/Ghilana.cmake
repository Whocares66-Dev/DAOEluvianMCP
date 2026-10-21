# ghilana.exe -- the MCP server of the tools that need no game (#128,
# dev/GHILANA.md): src/ghilana over el_core (MCP, the tool registry, the
# stdio transport) and DAOLib's daolib_core (the game's formats). Included by
# CMakeLists.txt after DAOLib is added, in the plugin presets (x86, so the
# debug gate tests and lints it) and the ghilana presets (x64, what ships).

# The names of GFF4 fields and GDA columns, from DAOLib's tables
# (ghilana/Names.h), written at configure into ghilana_names.inc.
set(_gh_fields_csv "${EL_DAOLIB}/dev/gff4_fields.csv")
set(_gh_columns_csv "${EL_DAOLIB}/dev/gda_tables.csv")
set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gh_fields_csv}" "${_gh_columns_csv}"
)
file(STRINGS "${_gh_fields_csv}" _gh_field_lines REGEX "^[0-9]+,")
set(_gh_names
    "// Written by cmake/Ghilana.cmake from DAOLib's dev/gff4_fields.csv and\n// dev/gda_tables.csv: do not edit.\n"
)
string(APPEND _gh_names "constexpr FieldName kGff4Fields[] = {\n")
foreach(_line IN LISTS _gh_field_lines)
    string(
        REGEX REPLACE "^([0-9]+),(.*)$"
        "    {\\1u, \"\\2\"},\n"
        _entry
        "${_line}"
    )
    string(APPEND _gh_names "${_entry}")
endforeach()
string(APPEND _gh_names "};\n")
# Each table's line ends with its columns, "Name:type Name:type ...", a name
# nothing knows written "#hash".
file(STRINGS "${_gh_columns_csv}" _gh_table_lines)
list(REMOVE_AT _gh_table_lines 0)
set(_gh_columns "")
foreach(_line IN LISTS _gh_table_lines)
    string(REGEX REPLACE "^.*,([^,]*)$" "\\1" _cols "${_line}")
    string(REPLACE " " ";" _cols "${_cols}")
    foreach(_col IN LISTS _cols)
        string(REGEX REPLACE ":[^:]*$" "" _name "${_col}")
        if(_name AND NOT _name MATCHES "^#")
            list(APPEND _gh_columns "${_name}")
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES _gh_columns)
list(SORT _gh_columns)
string(APPEND _gh_names "constexpr const char *kGdaColumns[] = {\n")
foreach(_name IN LISTS _gh_columns)
    string(APPEND _gh_names "    \"${_name}\",\n")
endforeach()
string(APPEND _gh_names "};\n")
file(
    CONFIGURE
    OUTPUT "${CMAKE_BINARY_DIR}/ghilana/ghilana_names.inc"
    CONTENT "${_gh_names}"
    @ONLY
)

# gh_lib: the tools and what they read with, everything but main, so the
# tests link it.
add_library(
    gh_lib
    STATIC
    src/ghilana/FileJson.cpp
    src/ghilana/Folders.cpp
    src/ghilana/GameFiles.cpp
    src/ghilana/Ghilana.cpp
    src/ghilana/Gltf.cpp
    src/ghilana/GltfImport.cpp
    src/ghilana/GltfMath.cpp
    src/ghilana/Names.cpp
    src/ghilana/Png.cpp
    src/ghilana/Steam.cpp
    src/ghilana/Tables.cpp
    src/ghilana/Text.cpp
    src/ghilana/ToolsData.cpp
    src/ghilana/ToolsFiles.cpp
    src/ghilana/ToolsLevels.cpp
    src/ghilana/ToolsModels.cpp
    src/ghilana/ToolsMods.cpp
    src/ghilana/ToolsTemplates.cpp
    src/ghilana/ToolsTextures.cpp
)
el_apply_warnings(gh_lib)
target_include_directories(gh_lib PRIVATE "${CMAKE_BINARY_DIR}/ghilana")
# Folders.cpp's registry (advapi32) and known folders (shell32, ole32);
# libspng for a texture's PNG (Png.cpp), from vcpkg.
find_package(SPNG CONFIG REQUIRED)
target_link_libraries(
    gh_lib
    PUBLIC
        el_core
        DAOLib::core
        advapi32
        shell32
        ole32
        $<IF:$<TARGET_EXISTS:spng::spng>,spng::spng,spng::spng_static>
)

add_executable(ghilana src/ghilana/Main.cpp)
el_apply_warnings(ghilana)
target_link_libraries(ghilana PRIVATE gh_lib)
target_compile_definitions(
    ghilana
    PRIVATE EL_VERSION_STRING="${PROJECT_VERSION}"
)
# A release's exe as eluvian.dll is linked (CMakeLists.txt): not
# incremental, unreferenced code dropped, a PDB of its own each link.
if(MSVC)
    target_link_options(
        ghilana
        PRIVATE "$<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF>"
    )
    if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        add_custom_command(
            TARGET ghilana
            PRE_LINK
            COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_PDB_FILE:ghilana>
            VERBATIM
        )
    endif()
endif()

# Its tests are tests/ghilana, added from tests/CMakeLists.txt, where Catch2
# is found.
if(TARGET gh_tests)
    # DAOLib's tests/files_builder.h: ghilana's tests make the game's files
    # as DAOLib's tests of the readers do.
    target_include_directories(gh_tests PRIVATE "${EL_DAOLIB}/tests")
endif()
