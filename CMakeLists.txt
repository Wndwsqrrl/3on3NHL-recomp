# nhl3on3 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nhl3on3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -O3 breaks the in-game scoreboard; Release uses -O2 like RelWithDebInfo, which renders correctly.
string(REPLACE "-O3" "-O2" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

include(generated/rexglue.cmake)

# Sources
set(NHL3ON3_SOURCES
    src/main.cpp
    src/kernel_stubs.cpp
)

if(WIN32)
    add_executable(nhl3on3 WIN32 ${NHL3ON3_SOURCES})
else()
    add_executable(nhl3on3 ${NHL3ON3_SOURCES})
endif()

rexglue_setup_target(nhl3on3 GPU_PLUGINS xenos)

# Default settings, read by the runtime from the executable's folder.
add_custom_command(TARGET nhl3on3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/nhl3on3.toml
        $<TARGET_FILE_DIR:nhl3on3>/nhl3on3.toml
    VERBATIM
)
