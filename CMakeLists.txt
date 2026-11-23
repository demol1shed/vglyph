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
  target_compile_options(vglyph-options INTERFACE /W4 /WX /EHs-c- /GR-)
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

add_library(vglyph-core STATIC
  src/core/convert.cpp
  src/core/utf8.cpp
  src/core/version.cpp
  src/media/ffmpeg_error.cpp
  src/media/media_reader.cpp
  src/render/text_serializer.cpp)
target_include_directories(vglyph-core PUBLIC include PRIVATE src/core src/media)
target_link_libraries(vglyph-core PRIVATE vglyph-options vglyph-ffmpeg)

add_executable(vglyph src/cli/main.cpp)
target_link_libraries(vglyph PRIVATE vglyph-core vglyph-options)

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

# Tests of internal (non-installed) headers need the private include dirs.
add_vglyph_test(ffmpeg_error)
target_include_directories(vglyph-ffmpeg_error-test PRIVATE src/media)

# Builds fixtures with FFmpeg's encoders, so it needs FFmpeg's headers directly.
add_vglyph_test(media_reader)
target_include_directories(vglyph-media_reader-test PRIVATE src/media)
target_link_libraries(vglyph-media_reader-test PRIVATE vglyph-ffmpeg)
