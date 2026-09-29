# nhl3on3 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nhl3on3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

rexglue_setup_target(nhl3on3)
