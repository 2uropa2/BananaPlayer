cmake_minimum_required(VERSION 3.21)
project(BananaIntegration LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(BP_DEFAULT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BP_SOURCE_DIR "${BP_DEFAULT_SOURCE}" CACHE PATH "Player source under test")
set(BP_DEPENDENCIES "${BP_DEFAULT_SOURCE}/build/vcpkg_installed/x64-windows" CACHE PATH "Built dependencies")
add_executable(banana_integration main.cpp
    "${BP_SOURCE_DIR}/VideoPlayer.cpp"
    "${BP_SOURCE_DIR}/Thumbnail.cpp"
    "${BP_SOURCE_DIR}/License.cpp"
    "${BP_SOURCE_DIR}/Subtitles.cpp")
# The only substitution in production code is the remote endpoint. The
# before/after comparison uses the same loopback server, never a live service.
file(READ "${BP_SOURCE_DIR}/Promo.cpp" BP_PROMO_SOURCE)
string(REPLACE "\"https://banana-player.com/promo.json\"" "std::getenv(\"BP_TEST_PROMO_URL\")" BP_PROMO_SOURCE "${BP_PROMO_SOURCE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/promo_under_test.cpp" "#include <cstdlib>\n${BP_PROMO_SOURCE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BP_SOURCE_DIR}/Promo.cpp")
target_sources(banana_integration PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/promo_under_test.cpp")
target_include_directories(banana_integration PRIVATE "${BP_SOURCE_DIR}" "${BP_DEPENDENCIES}/include")
if(EXISTS "${BP_SOURCE_DIR}/MediaExport.cpp")
    target_sources(banana_integration PRIVATE "${BP_SOURCE_DIR}/MediaExport.cpp" "${BP_SOURCE_DIR}/ExportJob.cpp")
    target_compile_definitions(banana_integration PRIVATE BP_HAS_EXPORT_JOB)
endif()
target_compile_definitions(banana_integration PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_definitions(banana_integration PRIVATE
    BP_LICENSE_PUBLIC_KEY_HEADER="${CMAKE_CURRENT_LIST_DIR}/../fixtures/license_public_key.hpp"
    BP_TEST_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/../fixtures")
if(MSVC)
    target_compile_options(banana_integration PRIVATE /utf-8 /W3 /permissive-)
endif()
foreach(lib SDL3 avcodec avformat avfilter avutil swscale swresample)
    target_link_libraries(banana_integration PRIVATE "${BP_DEPENDENCIES}/lib/${lib}.lib")
endforeach()
target_link_libraries(banana_integration PRIVATE user32 shell32 ole32)
file(GLOB BP_RUNTIME_DLLS "${BP_DEPENDENCIES}/bin/*.dll")
foreach(dll IN LISTS BP_RUNTIME_DLLS)
    add_custom_command(TARGET banana_integration POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:banana_integration>")
endforeach()
