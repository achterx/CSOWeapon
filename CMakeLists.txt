cmake_minimum_required(VERSION 3.15)
project(csnz_weapons)
set(CMAKE_CXX_STANDARD 17)

add_library(csnz_weapons SHARED
    src/dllmain.cpp
    src/hooks.cpp
    src/logger.cpp
    src/weapons/janus1.cpp
)

target_include_directories(csnz_weapons PRIVATE src)

set_target_properties(csnz_weapons PROPERTIES PREFIX "" OUTPUT_NAME "csnz_weapons")

if(MSVC)
    target_compile_options(csnz_weapons PRIVATE /W3 /EHa)
    target_link_options(csnz_weapons PRIVATE /MACHINE:X86)
endif()
