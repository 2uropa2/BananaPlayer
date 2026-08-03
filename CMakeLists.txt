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

project(BananaPlayer VERSION 1.4.0 LANGUAGES CXX)

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
    libavfilter
    libavutil
    libswscale
    libswresample
)

set(BP_SOURCES
    main.cpp
    VideoPlayer.cpp
    UIController.cpp
    Style.cpp
    Subtitles.cpp
    Thumbnail.cpp
    Youtube.cpp
    SingleInstance.cpp
    Promo.cpp
    License.cpp
)

# On Windows, compile the .rc so Explorer/taskbar pick up icon + version info.
if(WIN32)
    enable_language(RC)
    list(APPEND BP_SOURCES resources/app.rc)
endif()

add_executable(BananaPlayer ${BP_SOURCES})

# In non-Debug configs, use the WINDOWS subsystem so the console window
# doesn't tag along with the GUI. Debug keeps the console for stdout/stderr.
# SDL_main.h provides the WinMain shim that forwards to our main().
if(WIN32)
    set_property(TARGET BananaPlayer PROPERTY
        WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
endif()

target_link_libraries(BananaPlayer PRIVATE
    SDL3::SDL3
    imgui::imgui
    PkgConfig::FFMPEG
)

if(WIN32)
    # File open dialog (GetOpenFileName) lives here.
    target_link_libraries(BananaPlayer PRIVATE comdlg32 shell32 ole32)
endif()

# Copy SDL3 runtime DLL next to the built executable so it launches out-of-tree.
if(WIN32 AND TARGET SDL3::SDL3-shared)
    add_custom_command(TARGET BananaPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3-shared>
                $<TARGET_FILE_DIR:BananaPlayer>
        VERBATIM)
endif()

# -------- yt-dlp bundle --------
# YouTube URL playback needs yt-dlp. We fetch the latest standalone .exe
# from GitHub Releases at configure time and drop it next to BananaPlayer.exe
# so YoutubeResolver's portable-drop lookup finds it without a PATH probe.
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
    add_custom_command(TARGET BananaPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${YT_DLP_EXE}"
                $<TARGET_FILE_DIR:BananaPlayer>/yt-dlp.exe
        VERBATIM)
endif()

# -------- Install rules --------
install(TARGETS BananaPlayer RUNTIME DESTINATION bin)
if(WIN32)
    # vcpkg's applocal deploy step drops every runtime DLL the exe needs
    # (SDL3, FFmpeg's avcodec/avformat/swscale/swresample/avutil, ...) next
    # to the built exe. Scoop them all up rather than trying to enumerate
    # them by name - TARGET_RUNTIME_DLLS misses pkg-config imports.
    install(DIRECTORY $<TARGET_FILE_DIR:BananaPlayer>/
            DESTINATION bin
            FILES_MATCHING PATTERN "*.dll")
    # Bundle just the VC++ runtime (msvcp140.dll, vcruntime140.dll). Win10+
    # already ships the UCRT api-ms-win-* stubs system-wide.
    include(InstallRequiredSystemLibraries)
    install(PROGRAMS ${CMAKE_INSTALLED_SYSTEM_RUNTIME_LIBS} DESTINATION bin)
endif()
install(FILES resources/icon.ico DESTINATION share/icons)
# Ship the licenses alongside the exe so redistributors and end users
# both find them in one place.
install(FILES LICENSE LICENSE-3RDPARTY DESTINATION share)
# yt-dlp bundle - shipped as a PROGRAM so NSIS gives it +x on install and
# strips it on uninstall. Only installed if the download succeeded at
# configure time.
if(WIN32 AND EXISTS "${YT_DLP_EXE}")
    install(PROGRAMS "${YT_DLP_EXE}" DESTINATION bin)
endif()

# -------- CPack: installer + zip --------
set(CPACK_PACKAGE_NAME              "Banana Player")
set(CPACK_PACKAGE_VENDOR            "Banana Player")
set(CPACK_PACKAGE_VERSION_MAJOR     ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR     ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH     ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Banana Player - peel and play. Lightweight, keyboard-first video player (SDL3 + FFmpeg + Dear ImGui).")
# Install directory. Fresh installs land here; the app's runtime migration
# (see UIController::migrateSidecarsToDataDir) copies any legacy per-user
# data from %APPDATA%\Vela into %APPDATA%\BananaPlayer on first launch,
# so upgrading users don't lose their resume positions / stats / config.
set(CPACK_PACKAGE_INSTALL_DIRECTORY "BananaPlayer")
# Shortcut source (BananaPlayer.exe) -> display label the user sees in
# Start Menu / Desktop ("Banana Player").
set(CPACK_PACKAGE_EXECUTABLES       "BananaPlayer;Banana Player")
set(CPACK_CREATE_DESKTOP_LINKS      "BananaPlayer")

if(WIN32)
    # NSIS (needs makensis on PATH; ZIP always works as a portable fallback).
    set(CPACK_GENERATOR                       "ZIP;NSIS")
    set(CPACK_NSIS_DISPLAY_NAME               "Banana Player")
    set(CPACK_NSIS_PACKAGE_NAME               "Banana Player")
    set(CPACK_NSIS_MUI_ICON                   "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico")
    set(CPACK_NSIS_MUI_UNIICON                "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME        "bin\\\\BananaPlayer.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH                OFF)

    # File associations: register the exe under HKCR\Applications so
    # Explorer can locate + invoke it, and add it to each video ext's
    # OpenWithList so "Open with -> Banana Player" appears in the
    # right-click menu without forcing us as the default handler.
    #
    # The commands live in resources/banana_reg_{install,uninstall}.nsh
    # and are pulled in via NSIS `!include`. See the comment block that
    # explains why we can't inline these into CPACK_NSIS_EXTRA_INSTALL_
    # COMMANDS (the CPack round-trip strips the inner quote characters
    # required by `"$INSTDIR\bin\BananaPlayer.exe" "%1"`).
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/banana_reg_install.nsh"
         BP_REG_INSTALL_NSH)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/banana_reg_uninstall.nsh"
         BP_REG_UNINSTALL_NSH)
    # Pre-double the backslashes so the on-disk CPackConfig.cmake has `\\`
    # which CMake reads back as a single `\` in the final NSIS command
    # (a single raw `\` would parse as an invalid character escape).
    string(REPLACE "\\" "\\\\" BP_REG_INSTALL_NSH   "${BP_REG_INSTALL_NSH}")
    string(REPLACE "\\" "\\\\" BP_REG_UNINSTALL_NSH "${BP_REG_UNINSTALL_NSH}")
    if(BP_REG_INSTALL_NSH MATCHES " ")
        message(FATAL_ERROR
            "The source path contains a space: ${CMAKE_CURRENT_SOURCE_DIR}. "
            "The NSIS !include mechanism used for the file-association "
            "installer commands can't be quoted here (see CMakeLists.txt "
            "for details); move the checkout to a path without spaces.")
    endif()
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
        "  !include ${BP_REG_INSTALL_NSH}")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
        "  !include ${BP_REG_UNINSTALL_NSH}")
else()
    set(CPACK_GENERATOR "ZIP")
endif()

include(CPack)

# -------- Optional unit tests (doctest) --------
option(BP_BUILD_TESTS "Build unit tests" ON)
if(BP_BUILD_TESTS)
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
    add_executable(banana_tests
        tests/test_main.cpp
        tests/test_subtitles.cpp
        Subtitles.cpp
    )
    target_include_directories(banana_tests PRIVATE
        . ${CMAKE_CURRENT_SOURCE_DIR})
    # doctest.h is downloaded flat, but tests include it as "doctest/doctest.h"
    # - copy it into a namespaced subdir so the include path resolves.
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/doctest_include/doctest)
    file(COPY ${DOCTEST_HEADER_DIR}/doctest.h
         DESTINATION ${CMAKE_BINARY_DIR}/doctest_include/doctest)
    target_include_directories(banana_tests PRIVATE
        ${CMAKE_BINARY_DIR}/doctest_include)
    if(MSVC)
        target_compile_options(banana_tests PRIVATE /W3 /permissive- /Zc:__cplusplus)
        target_compile_definitions(banana_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    endif()
    add_test(NAME banana_tests COMMAND banana_tests)
endif()
