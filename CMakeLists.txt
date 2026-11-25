cmake_minimum_required(VERSION 3.25)
project(vglyph VERSION 0.0.0 LANGUAGES CXX)

option(VGLYPH_BUILD_GUI "Build the vglyph-gui desktop executable" OFF)
option(VGLYPH_ENABLE_SANITIZERS "Enable ASan + UBSan (GCC/Clang only)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Shared compile options for every vglyph target (no exceptions, no RTTI).
add_library(vglyph-options INTERFACE)
if(MSVC)
  # /external:W0 silences SYSTEM headers (FFmpeg, third_party) under /W4 /WX.
  target_compile_options(vglyph-options INTERFACE /W4 /WX /EHs-c- /GR- /external:W0)
  target_compile_definitions(vglyph-options INTERFACE _HAS_EXCEPTIONS=0)
else()
  target_compile_options(vglyph-options INTERFACE
    -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow
    -fno-exceptions -fno-rtti)
  if(VGLYPH_ENABLE_SANITIZERS)
    target_compile_options(vglyph-options INTERFACE
      -fsanitize=address,undefined -fno-sanitize-recover=all
      -fno-omit-frame-pointer)
    target_link_options(vglyph-options INTERFACE
      -fsanitize=address,undefined)
  endif()
endif()

# FFmpeg: vcpkg (Windows CI, `media` feature) or the system packages through
# pkg-config (Linux). Exposed as one INTERFACE target; headers are SYSTEM so
# -Werror never applies to them.
add_library(vglyph-ffmpeg INTERFACE)
if(VCPKG_TOOLCHAIN)
  find_package(FFMPEG REQUIRED)
  target_include_directories(vglyph-ffmpeg SYSTEM INTERFACE ${FFMPEG_INCLUDE_DIRS})
  target_link_directories(vglyph-ffmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(vglyph-ffmpeg INTERFACE ${FFMPEG_LIBRARIES})
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libswscale libavutil)
  target_link_libraries(vglyph-ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

# The default font is embedded as a generated header (SPEC 7.1), so a build
# needs no font file at run time.
set(VGLYPH_FONT_FILE ${CMAKE_SOURCE_DIR}/assets/fonts/JetBrainsMono-Regular.ttf)
set(VGLYPH_FONT_HEADER ${CMAKE_BINARY_DIR}/generated/vglyph_embedded_font.hpp)
add_custom_command(
  OUTPUT ${VGLYPH_FONT_HEADER}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${VGLYPH_FONT_FILE} -DOUTPUT=${VGLYPH_FONT_HEADER}
          -P ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
  DEPENDS ${VGLYPH_FONT_FILE} ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
  COMMENT "Embedding the default font")

add_library(vglyph-core STATIC
  ${VGLYPH_FONT_HEADER}
  src/core/convert.cpp
  src/core/utf8.cpp
  src/core/version.cpp
  src/media/ffmpeg_error.cpp
  src/media/media_exporter.cpp
  src/media/quantize.cpp
  src/media/media_reader.cpp
  src/render/glyph_atlas.cpp
  src/render/rasterizer.cpp
  src/render/text_serializer.cpp)
target_include_directories(vglyph-core PUBLIC include PRIVATE src/core src/media)
# Vendored single headers are SYSTEM, so -Werror never applies to them.
target_include_directories(vglyph-core SYSTEM PRIVATE third_party)
target_include_directories(vglyph-core PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(vglyph-core PRIVATE vglyph-options vglyph-ffmpeg)
if(MSVC)
  # stb_truetype.h has non-ASCII characters in two comments.
  set_source_files_properties(src/render/glyph_atlas.cpp PROPERTIES COMPILE_OPTIONS /utf-8)
endif()

# CLI logic as a static library so the tests can link it; main.cpp stays tiny.
# platform_*.cpp are the only files that include OS console headers.
add_library(vglyph-cli-impl STATIC
  src/cli/args.cpp
  src/cli/commands.cpp
  src/cli/player.cpp)
if(WIN32)
  target_sources(vglyph-cli-impl PRIVATE src/cli/platform_win32.cpp)
else()
  target_sources(vglyph-cli-impl PRIVATE src/cli/platform_posix.cpp)
endif()
target_include_directories(vglyph-cli-impl PUBLIC src/cli)
target_link_libraries(vglyph-cli-impl PUBLIC vglyph-core PRIVATE vglyph-options)

add_executable(vglyph src/cli/main.cpp)
target_link_libraries(vglyph PRIVATE vglyph-cli-impl vglyph-options)
if(MSVC)
  # UTF-8 active code page: argv and file names arrive as UTF-8 on Windows.
  target_sources(vglyph PRIVATE src/cli/vglyph.manifest)
endif()

if(VGLYPH_BUILD_GUI)
  # Phase 0 stub: SDL3 / Dear ImGui are linked in Phase 6.
  add_executable(vglyph-gui src/gui/main.cpp)
  target_link_libraries(vglyph-gui PRIVATE vglyph-core vglyph-options)
endif()

enable_testing()
add_executable(vglyph-smoke-test tests/smoke_test.cpp)
target_link_libraries(vglyph-smoke-test PRIVATE vglyph-core vglyph-options)
add_test(NAME smoke COMMAND vglyph-smoke-test)

# Registers tests/<name>_test.cpp as the CTest test "<name>".
function(add_vglyph_test name)
  add_executable(vglyph-${name}-test tests/${name}_test.cpp)
  target_link_libraries(vglyph-${name}-test PRIVATE vglyph-core vglyph-options)
  add_test(NAME ${name} COMMAND vglyph-${name}-test)
endfunction()

add_vglyph_test(error)
add_vglyph_test(grid)
add_vglyph_test(convert)
add_vglyph_test(utf8)
add_vglyph_test(text_serializer)

# Replaces the global operator new to count allocations in the rasterizer loop.
add_vglyph_test(rasterizer)

# Tests of internal (non-installed) headers need the private include dirs.
add_vglyph_test(ffmpeg_error)
target_include_directories(vglyph-ffmpeg_error-test PRIVATE src/media)

# Builds fixtures with FFmpeg's encoders, so it needs FFmpeg's headers directly.
add_vglyph_test(media_reader)
target_include_directories(vglyph-media_reader-test PRIVATE src/media)
target_link_libraries(vglyph-media_reader-test PRIVATE vglyph-ffmpeg)

# CLI tests link the CLI library (which brings in its own include directory).
add_vglyph_test(cli_args)
target_link_libraries(vglyph-cli_args-test PRIVATE vglyph-cli-impl)

# Writes fixtures with FFmpeg's encoders, like the media_reader test.
add_vglyph_test(cli_convert)
target_include_directories(vglyph-cli_convert-test PRIVATE src/media)
target_link_libraries(vglyph-cli_convert-test PRIVATE vglyph-cli-impl vglyph-ffmpeg)

# Replaces the global operator new to count allocations in the frame hot path.
add_vglyph_test(player)
target_link_libraries(vglyph-player-test PRIVATE vglyph-cli-impl)
