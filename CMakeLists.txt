cmake_minimum_required(VERSION 3.20)

project(RemoteMouseFix
    VERSION 0.2.0
    DESCRIPTION "Diagnostics and guarded correction for mouse/camera jumps in 3D games over remote desktop software"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "RemoteMouseFix targets Windows only.\n"
        "Do not build this with a native Linux compiler. Either configure with MSVC on "
        "Windows, or cross-compile from WSL using:\n"
        "  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64-x64.cmake")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# x64 only for now.
if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "RemoteMouseFix currently supports x64 only (got ${CMAKE_SIZEOF_VOID_P}-byte pointers).")
endif()

option(RMF_BUILD_PROBE "Build MouseLookProbe, the stand-in game used to test the correction" ON)

configure_file(cmake/Version.h.in "${CMAKE_BINARY_DIR}/generated/rmf/Version.h" @ONLY)

# Settings shared by every executable: same defines, same warnings, static runtime so each
# EXE stays a single portable file.
function(rmf_apply_common_settings target)
    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated")

    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00      # Windows 10
        WINVER=0x0A00)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O2>)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endfunction()

add_executable(RemoteMouseFix
    src/main.cpp
    src/Config.cpp
    src/Correction.cpp
    src/EventRecord.cpp
    src/Logger.cpp
    src/MouseHook.cpp
    src/ProcessInfo.cpp
    src/StateSampler.cpp
    src/WinCompat.cpp
)
rmf_apply_common_settings(RemoteMouseFix)
target_link_libraries(RemoteMouseFix PRIVATE user32 gdi32 shell32 shcore)

# Ship both configs next to the EXE so the build output is directly usable.
add_custom_command(TARGET RemoteMouseFix POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/config.json"
            "${CMAKE_SOURCE_DIR}/config/config-ab-test.json"
            "$<TARGET_FILE_DIR:RemoteMouseFix>"
    COMMENT "Copying config files next to RemoteMouseFix.exe")

if(RMF_BUILD_PROBE)
    add_executable(MouseLookProbe WIN32
        tools/probe/MouseLookProbe.cpp
        src/WinCompat.cpp
    )
    rmf_apply_common_settings(MouseLookProbe)
    target_link_libraries(MouseLookProbe PRIVATE user32 gdi32 shell32)
endif()

install(TARGETS RemoteMouseFix RUNTIME DESTINATION .)
install(FILES config/config.json config/config-ab-test.json README.md LICENSE DESTINATION .)
install(FILES docs/diagnostics.md DESTINATION docs)
