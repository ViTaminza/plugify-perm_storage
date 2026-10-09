# perm_storage — Plugify C++ plugin: persistence for the official "permissions" plugin.
#
#   perm_storage_core      engine-independent storage core (also used by tests/perm_storage_tests.cpp)
#   perm_storage_config    the databases.cfg reader (PLUGIN_STANDARD.md section 4)
#   plugify-perm_storage   the plugin shared library (entry "bin/perm_storage" -> bin/libperm_storage.so)

set(PERM_STORAGE_VERSION "1.0.0")

# --- core ---------------------------------------------------------------------
add_library(perm_storage_core STATIC src/perm_storage_core.cpp)
target_include_directories(perm_storage_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(perm_storage_core PUBLIC plgdb)
target_compile_options(perm_storage_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(perm_storage_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- database settings ---------------------------------------------------------
# The reader of <GetConfigsDir()>/perm_storage/databases.cfg, section "perm_storage"
# (PLUGIN_STANDARD.md section 4): databases.cfg first, the legacy keys of config.jsonc - with one
# warning - while the section does not exist (src/db_config.hpp). Separate from perm_storage_core so
# the storage core stays free of plugify/pisexcompat dependencies, and separate from plugin.cpp so
# tests/db_config_tests.cpp can drive the precedence rules without an engine.
#
# perm_storage is engine-agnostic - its manifest depends on the permissions plugin only - so instead
# of linking the whole pisexcompat, whose PUBLIC link would also compile the s2sdk/hudmenu binding
# globals into this .so, only its KeyValues parser and the config/data path mapping are built here.
add_library(perm_storage_config STATIC
        src/db_config.cpp
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/src/keyvalues.cpp"
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/src/paths.cpp")
target_include_directories(perm_storage_config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the pisexcompat and generated sdk headers must not fail -Werror on our sources.
target_include_directories(perm_storage_config SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(perm_storage_config PUBLIC plgdb)
# The same flags libs/pisexcompat compiles its own sources with (its headers are not ours to fix).
target_compile_options(perm_storage_config PRIVATE -Wall -Wextra -Werror -fvisibility=hidden)
target_compile_definitions(perm_storage_config PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-perm_storage SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:permissions_bindings>
)
target_include_directories(plugify-perm_storage PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated permissions/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-perm_storage SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-perm_storage PRIVATE
        perm_storage_core
        perm_storage_config
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-perm_storage PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-perm_storage PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="perm_storage"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-perm_storage PROPERTIES OUTPUT_NAME perm_storage)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-perm_storage PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/perm_storage.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/perm_storage.pplugin"
)

# --- tests ---------------------------------------------------------------------
# Databases.cfg precedence and the transition-period fallback (tests/db_config_tests.cpp); run by
# ../test.sh from build/tests like every other *_tests executable. The storage core's own tests stay
# in ../tests/perm_storage_tests.cpp.
add_executable(perm_storage_dbconfig_tests tests/db_config_tests.cpp)
target_include_directories(perm_storage_dbconfig_tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(perm_storage_dbconfig_tests PRIVATE perm_storage_config -static-libstdc++ -static-libgcc)
target_compile_options(perm_storage_dbconfig_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(perm_storage_dbconfig_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
set_target_properties(perm_storage_dbconfig_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
