cmake_minimum_required(VERSION 3.20)

project(RemoteMouseFix
    VERSION 0.1.0
    DESCRIPTION "Diagnostic tool for mouse/camera jumps in 3D games over remote desktop software"
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

# x64 only for now. Phase 1 has no 32-bit target.
if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "RemoteMouseFix currently supports x64 only (got ${CMAKE_SIZEOF_VOID_P}-byte pointers).")
endif()

add_executable(RemoteMouseFix
    src/main.cpp
    src/Config.cpp
    src/EventRecord.cpp
    src/Logger.cpp
    src/MouseHook.cpp
    src/ProcessInfo.cpp
    src/StateSampler.cpp
    src/WinCompat.cpp
)

configure_file(cmake/Version.h.in "${CMAKE_BINARY_DIR}/generated/rmf/Version.h" @ONLY)

target_include_directories(RemoteMouseFix PRIVATE include "${CMAKE_BINARY_DIR}/generated")

target_compile_definitions(RemoteMouseFix PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00      # Windows 10
    WINVER=0x0A00
)

target_link_libraries(RemoteMouseFix PRIVATE user32 shell32 shcore)

if(MSVC)
    target_compile_options(RemoteMouseFix PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_compile_options(RemoteMouseFix PRIVATE $<$<CONFIG:Release>:/O2>)
    # Static CRT keeps the tool portable: a single EXE, no redistributable needed.
    set_property(TARGET RemoteMouseFix PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(RemoteMouseFix PRIVATE -Wall -Wextra -Wpedantic)
    # Fully static link so the EXE runs on any Windows 10/11 box without MinGW DLLs.
    target_link_options(RemoteMouseFix PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Console subsystem on purpose: the operator watches live status output.
set_target_properties(RemoteMouseFix PROPERTIES
    OUTPUT_NAME "RemoteMouseFix"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Ship config.json next to the EXE so the build output is directly portable.
add_custom_command(TARGET RemoteMouseFix POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/config.json"
            "$<TARGET_FILE_DIR:RemoteMouseFix>/config.json"
    COMMENT "Copying config/config.json next to RemoteMouseFix.exe")

install(TARGETS RemoteMouseFix RUNTIME DESTINATION .)
install(FILES config/config.json README.md LICENSE DESTINATION .)
install(FILES docs/diagnostics.md DESTINATION docs)
