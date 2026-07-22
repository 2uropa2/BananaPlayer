cmake_minimum_required(VERSION 3.21)

# Point CMake at vcpkg's toolchain BEFORE project() when not already set.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain file")
    elseif(EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain file")
    endif()
endif()

if(NOT DEFINED VCPKG_TARGET_TRIPLET AND WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet")
endif()

# Set compiler for MSVC
if(MSVC)
    set(CMAKE_CXX_COMPILER "cl" CACHE STRING "MSVC C++ compiler" FORCE)
endif()

project(Vela VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W3 /permissive- /Zc:__cplusplus)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
    libswresample
)

set(VELA_SOURCES
    main.cpp
    VideoPlayer.cpp
    UIController.cpp
    Style.cpp
    Subtitles.cpp
    Thumbnail.cpp
    Youtube.cpp
)

# On Windows, compile the .rc so Explorer/taskbar pick up icon + version info.
if(WIN32)
    enable_language(RC)
    list(APPEND VELA_SOURCES resources/app.rc)
endif()

add_executable(Vela ${VELA_SOURCES})

# In non-Debug configs, use the WINDOWS subsystem so the console window
# doesn't tag along with the GUI. Debug keeps the console for stdout/stderr.
# SDL_main.h provides the WinMain shim that forwards to our main().
if(WIN32)
    set_property(TARGET Vela PROPERTY
        WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
endif()

target_link_libraries(Vela PRIVATE
    SDL3::SDL3
    imgui::imgui
    PkgConfig::FFMPEG
)

if(WIN32)
    # File open dialog (GetOpenFileName) lives here.
    target_link_libraries(Vela PRIVATE comdlg32)
endif()

# Copy SDL3 runtime DLL next to the built executable so it launches out-of-tree.
if(WIN32 AND TARGET SDL3::SDL3-shared)
    add_custom_command(TARGET Vela POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3-shared>
                $<TARGET_FILE_DIR:Vela>
        VERBATIM)
endif()

# -------- yt-dlp bundle --------
# YouTube URL playback needs yt-dlp. We fetch the latest standalone .exe
# from GitHub Releases at configure time and drop it next to Vela.exe so
# YoutubeResolver's portable-drop lookup finds it without a PATH probe.
# The installer (NSIS/ZIP) then picks it up via the install rule below.
#
# Refresh policy: cached in build/yt-dlp.exe; delete that file and re-run
# cmake to pull a newer release. YouTube changes internal protocols every
# few weeks and yt-dlp releases roll about as often - the in-app age nudge
# reminds users to update once shipped, but shipping fresh is still nice.
set(YT_DLP_EXE "${CMAKE_BINARY_DIR}/yt-dlp.exe")
if(WIN32 AND NOT EXISTS "${YT_DLP_EXE}")
    message(STATUS "Downloading latest yt-dlp.exe...")
    file(DOWNLOAD
        "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"
        "${YT_DLP_EXE}"
        SHOW_PROGRESS
        STATUS YT_DLP_DL_STATUS
        TIMEOUT 180)
    list(GET YT_DLP_DL_STATUS 0 YT_DLP_DL_CODE)
    if(NOT YT_DLP_DL_CODE EQUAL 0)
        message(WARNING "yt-dlp download failed (${YT_DLP_DL_STATUS}); YouTube URLs will need manual install")
        file(REMOVE "${YT_DLP_EXE}")
    endif()
endif()
if(WIN32 AND EXISTS "${YT_DLP_EXE}")
    add_custom_command(TARGET Vela POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${YT_DLP_EXE}"
                $<TARGET_FILE_DIR:Vela>/yt-dlp.exe
        VERBATIM)
endif()

# -------- Install rules --------
install(TARGETS Vela RUNTIME DESTINATION bin)
if(WIN32)
    # vcpkg's applocal deploy step drops every runtime DLL the exe needs
    # (SDL3, FFmpeg's avcodec/avformat/swscale/swresample/avutil, ...) next
    # to the built exe. Scoop them all up rather than trying to enumerate
    # them by name - TARGET_RUNTIME_DLLS misses pkg-config imports.
    install(DIRECTORY $<TARGET_FILE_DIR:Vela>/
            DESTINATION bin
            FILES_MATCHING PATTERN "*.dll")
    # Bundle just the VC++ runtime (msvcp140.dll, vcruntime140.dll). Win10+
    # already ships the UCRT api-ms-win-* stubs system-wide.
    include(InstallRequiredSystemLibraries)
    install(PROGRAMS ${CMAKE_INSTALLED_SYSTEM_RUNTIME_LIBS} DESTINATION bin)
endif()
install(FILES resources/icon.ico DESTINATION share/icons)
# yt-dlp bundle - shipped as a PROGRAM so NSIS gives it +x on install and
# strips it on uninstall. Only installed if the download succeeded at
# configure time.
if(WIN32 AND EXISTS "${YT_DLP_EXE}")
    install(PROGRAMS "${YT_DLP_EXE}" DESTINATION bin)
endif()

# -------- CPack: installer + zip --------
set(CPACK_PACKAGE_NAME              "Vela")
set(CPACK_PACKAGE_VENDOR            "Vela")
set(CPACK_PACKAGE_VERSION_MAJOR     ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR     ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH     ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Vela - minimal, keyboard-first video player (SDL3 + FFmpeg + Dear ImGui).")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Vela")
# Shows in Start Menu after install: "Vela".
set(CPACK_PACKAGE_EXECUTABLES       "Vela;Vela")
set(CPACK_CREATE_DESKTOP_LINKS      "Vela")

if(WIN32)
    # NSIS (needs makensis on PATH; ZIP always works as a portable fallback).
    set(CPACK_GENERATOR                       "ZIP;NSIS")
    set(CPACK_NSIS_DISPLAY_NAME               "Vela")
    set(CPACK_NSIS_PACKAGE_NAME               "Vela")
    set(CPACK_NSIS_MUI_ICON                   "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico")
    set(CPACK_NSIS_MUI_UNIICON                "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME        "bin\\\\Vela.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH                OFF)

    # File associations: register the exe under HKCR\Applications so
    # Explorer can locate + invoke it, and add it to each video ext's
    # OpenWithList so "Open with -> Vela" appears in the right-click menu
    # without forcing Vela as the default handler.
    #
    # The commands live in resources/vela_reg_{install,uninstall}.nsh and
    # are pulled in via NSIS `!include`. Why the indirection: writing the
    # WriteRegStr commands directly into CPACK_NSIS_EXTRA_INSTALL_COMMANDS
    # forces the literal `"` chars needed by `"$INSTDIR\bin\Vela.exe" "%1"`
    # through a CMake string round-trip (variable value -> CPackConfig.cmake
    # -> NSIS template substitution) that CPack does NOT escape - the inner
    # quote closes the outer CMake string early and the shell\open\command
    # value ends up unquoted, so Windows fails to launch Vela with "Open
    # with -> Vela". `!include` bypasses the round-trip entirely: NSIS
    # reads the raw .nsh at compile time.
    # NSIS's !include on Windows wants native backslashes - the .nsi file
    # otherwise sees "C:/work/..." and reports "could not find". Convert
    # both paths to native form and (defensively) require the source path
    # to be space-free so we don't need a quoted !include argument (which
    # would drag `"` chars through the CPackConfig.cmake round-trip that
    # eats them, breaking the very problem we're fixing).
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/vela_reg_install.nsh"
         VELA_REG_INSTALL_NSH)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/vela_reg_uninstall.nsh"
         VELA_REG_UNINSTALL_NSH)
    # CPack writes CPACK_NSIS_EXTRA_INSTALL_COMMANDS into CPackConfig.cmake
    # verbatim. If our value contains a raw `\`, the resulting file line
    #   set(... "  !include C:\work\Player\...")
    # trips CMake's "Invalid character escape '\w'" at re-parse time.
    # Pre-double the backslashes so the on-disk file has `\\` which
    # CMake reads back as a single `\` in the final NSIS command.
    string(REPLACE "\\" "\\\\" VELA_REG_INSTALL_NSH   "${VELA_REG_INSTALL_NSH}")
    string(REPLACE "\\" "\\\\" VELA_REG_UNINSTALL_NSH "${VELA_REG_UNINSTALL_NSH}")
    if(VELA_REG_INSTALL_NSH MATCHES " ")
        message(FATAL_ERROR
            "The source path contains a space: ${CMAKE_CURRENT_SOURCE_DIR}. "
            "The NSIS !include mechanism used for the file-association "
            "installer commands can't be quoted here (see CMakeLists.txt "
            "for details); move the checkout to a path without spaces.")
    endif()
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
        "  !include ${VELA_REG_INSTALL_NSH}")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
        "  !include ${VELA_REG_UNINSTALL_NSH}")
else()
    set(CPACK_GENERATOR "ZIP")
endif()

include(CPack)

# -------- Optional unit tests (doctest) --------
option(VELA_BUILD_TESTS "Build unit tests" ON)
if(VELA_BUILD_TESTS)
    # Fetch just the single-header doctest.h. Avoids issues with doctest's
    # own CMakeLists (which uses cmake_minimum_required 2.x that recent CMake
    # rejects), and keeps the test target self-contained.
    include(FetchContent)
    FetchContent_Declare(doctest_header
        URL https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h
        DOWNLOAD_NO_EXTRACT TRUE
        DOWNLOAD_DIR ${CMAKE_BINARY_DIR}/_deps/doctest_header
    )
    FetchContent_MakeAvailable(doctest_header)
    set(DOCTEST_HEADER_DIR "${CMAKE_BINARY_DIR}/_deps/doctest_header")

    enable_testing()
    add_executable(vela_tests
        tests/test_main.cpp
        tests/test_subtitles.cpp
        Subtitles.cpp
    )
    target_include_directories(vela_tests PRIVATE
        . ${CMAKE_CURRENT_SOURCE_DIR})
    # doctest.h is downloaded flat, but tests include it as "doctest/doctest.h"
    # - copy it into a namespaced subdir so the include path resolves.
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/doctest_include/doctest)
    file(COPY ${DOCTEST_HEADER_DIR}/doctest.h
         DESTINATION ${CMAKE_BINARY_DIR}/doctest_include/doctest)
    target_include_directories(vela_tests PRIVATE
        ${CMAKE_BINARY_DIR}/doctest_include)
    if(MSVC)
        target_compile_options(vela_tests PRIVATE /W3 /permissive- /Zc:__cplusplus)
        target_compile_definitions(vela_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    endif()
    add_test(NAME vela_tests COMMAND vela_tests)
endif()
