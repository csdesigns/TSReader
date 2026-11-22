# tsreader CLI — modern POSIX build (macOS / Linux).
#
# Standalone project so it does not interfere with the legacy MSVC
# CMakeLists.txt at the repository root:
#
#   cmake -S cli -B build-cli
#   cmake --build build-cli
#   ./build-cli/tsreader list test.ts
#
# Optional: link FFmpeg for codec-level probing (detected via
# pkg-config or Homebrew prefixes). The CLI works without it.

cmake_minimum_required(VERSION 3.16)
project(tsreader_cli C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_FLAGS_RELEASE "-O2 -g")
set(CMAKE_C_FLAGS_DEBUG "-O0 -g -DTSR_DEBUG")

add_executable(tsreader
    tsreader_cli.c
    tsreader_si.c
    tsreader_report.c
    tsreader_probe.c
    tsreader_hdhr.c
    tsreader_main.c
)
target_include_directories(tsreader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(tsreader PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type-mismatch
)
if(APPLE)
    target_compile_definitions(tsreader PRIVATE _DARWIN_C_SOURCE)
else()
    target_compile_definitions(tsreader PRIVATE _GNU_SOURCE)
endif()
find_library(PTHREAD_LIB pthread)
if(PTHREAD_LIB)
    target_link_libraries(tsreader PRIVATE ${PTHREAD_LIB})
endif()

# Optional FFmpeg integration (codec detail in `list` output).
set(TSR_FFMPEG_HAVES)
# Prefer Homebrew prefixes with full paths (avoids pkg-config -L quirks).
foreach(pfx /opt/homebrew /usr/local)
    if(EXISTS ${pfx}/lib/libavformat.dylib
       AND EXISTS ${pfx}/lib/libavcodec.dylib)
        set(AVFORMAT_FOUND TRUE)
        set(AVFORMAT_INCLUDE_DIRS ${pfx}/include)
        set(AVFORMAT_LIBRARIES ${pfx}/lib/libavformat.dylib)
        set(AVCODEC_FOUND TRUE)
        set(AVCODEC_INCLUDE_DIRS ${pfx}/include)
        set(AVCODEC_LIBRARIES ${pfx}/lib/libavcodec.dylib)
        set(AVUTIL_FOUND TRUE)
        set(AVUTIL_INCLUDE_DIRS ${pfx}/include)
        set(AVUTIL_LIBRARIES ${pfx}/lib/libavutil.dylib)
        break()
    endif()
endforeach()
if(NOT (AVCODEC_FOUND AND AVFORMAT_FOUND AND AVUTIL_FOUND))
    # Fallback: pkg-config.
    include(FindPkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(AVCODEC libavcodec)
        pkg_check_modules(AVFORMAT libavformat)
        pkg_check_modules(AVUTIL libavutil)
    endif()
endif()
if(AVCODEC_FOUND AND AVFORMAT_FOUND AND AVUTIL_FOUND)
    message(STATUS "tsreader: FFmpeg found — enabling codec probe")
    target_compile_definitions(tsreader PRIVATE TSR_HAVE_FFMPEG=1)
    target_include_directories(tsreader PRIVATE
        ${AVCODEC_INCLUDE_DIRS} ${AVFORMAT_INCLUDE_DIRS}
        ${AVUTIL_INCLUDE_DIRS})
    target_link_directories(tsreader PRIVATE
        ${AVCODEC_LIBRARY_DIRS} ${AVFORMAT_LIBRARY_DIRS}
        ${AVUTIL_LIBRARY_DIRS})
    target_link_libraries(tsreader PRIVATE
        ${AVCODEC_LIBRARIES} ${AVFORMAT_LIBRARIES} ${AVUTIL_LIBRARIES})
else()
    message(STATUS "tsreader: FFmpeg not found — codec probe disabled")
endif()

# Optional HDHomeRun (SiC) source: HTTP tune + transport stream.
# libcurl is in the macOS SDK and a standard Linux package.
find_package(CURL QUIET)
if(CURL_FOUND)
    message(STATUS "tsreader: libcurl found — enabling HDHomeRun source")
    target_compile_definitions(tsreader PRIVATE TSR_HAVE_CURL=1)
    target_link_libraries(tsreader PRIVATE CURL::libcurl)
else()
    message(STATUS "tsreader: libcurl not found — HDHomeRun source disabled")
endif()

install(TARGETS tsreader RUNTIME DESTINATION bin)
