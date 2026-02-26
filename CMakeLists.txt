cmake_minimum_required(VERSION 3.15)
project(frostbite_fix)

set(CMAKE_GENERATOR_PLATFORM Win32)

add_library(frostbite_fix SHARED
    src/dllmain.cpp
    src/frostbite_fix.cpp
    src/givefnptrs.cpp
)

target_include_directories(frostbite_fix PRIVATE src)

set_target_properties(frostbite_fix PROPERTIES
    CXX_STANDARD 17
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_options(frostbite_fix PRIVATE
    /W3
    /EHa
    /GS-
)

target_compile_definitions(frostbite_fix PRIVATE _CRT_SECURE_NO_WARNINGS)

target_link_libraries(frostbite_fix PRIVATE psapi)

# Ensure 32-bit
if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MACHINE:X86")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MACHINE:X86")
endif()
