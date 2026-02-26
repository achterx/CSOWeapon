cmake_minimum_required(VERSION 3.16)
project(frostbite_fix CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# HARD REQUIREMENT: 32-bit only.
# hlds.exe and mp.dll are x86. A 64-bit DLL will crash on load.
# ============================================================
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "frostbite_fix MUST be built as 32-bit.\n"
        "Run cmake with: -A Win32  (MSVC)  or  -m32  (GCC/Clang)\n"
        "Example: cmake -B build -A Win32")
endif()

add_library(frostbite_fix SHARED
    src/dllmain.cpp
    src/givefnptrs.cpp
    src/pattern_scan.cpp
    src/frostbite_fix.cpp
)

target_include_directories(frostbite_fix PRIVATE
    include
)

target_compile_definitions(frostbite_fix PRIVATE
    WIN32
    _WINDOWS
    _USRDLL
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(frostbite_fix PRIVATE
        /W3 /WX- /Oi /GS- /EHsc
        $<$<CONFIG:Release>:/O2 /Ob2 /DNDEBUG>
        $<$<CONFIG:Debug>:/Od /Zi /D_DEBUG>
    )
endif()

set_target_properties(frostbite_fix PROPERTIES PREFIX "")

target_link_libraries(frostbite_fix PRIVATE kernel32 user32)

add_custom_command(TARGET frostbite_fix POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Built: $<TARGET_FILE:frostbite_fix>"
)
