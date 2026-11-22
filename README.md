# vglyph

A fast, minimal, cross-platform ASCII art converter and player for photos, animated GIFs, and videos.

> **Status: pre-alpha.** The project is at Phase 0 (build skeleton). Everything under [Planned features](#planned-features) is specified in [`SPEC.md`](SPEC.md) but not implemented yet.

## Planned features

- **One decoding path:** PNG, JPEG, WebP, BMP, animated GIF, and common video containers (MP4, MKV, WebM, MOV, AVI) all go through FFmpeg.
- **Rendering modes:** classic luminance ramps, Unicode half-blocks (`▀`) for double vertical resolution, and 24-bit TrueColor foreground/background. Braille, Sobel edge detection, and dithering follow later.
- **Terminal playback:** flicker-free, PTS-paced playback with `vglyph play`.
- **Export:** text and ANSI (`.txt`, `.ans`), and rasterized `.png`, `.gif`, `.mp4` using an embedded OFL monospace font or your own `.ttf`.
- **Desktop GUI:** a lightweight SDL3 + Dear ImGui front end with live preview and non-blocking export.
- **Lean engineering:** C++20 without exceptions or RTTI, RAII around every C API, bounded streaming memory, and bit-identical output between CLI and GUI.

Platforms: Linux (x86_64 / aarch64) and Windows 11 (x86_64). A TrueColor (24-bit) terminal is required for `vglyph play` and `.ans` output; there is no 256-color fallback.

## Planned CLI

```sh
vglyph convert photo.jpg -o photo.ans -w 120 --mode halfblock
vglyph convert clip.mp4  -o clip.gif  --font-size 14
vglyph play    clip.mp4  --loop
vglyph info    clip.mp4
```

None of these commands exist yet; today only `vglyph --version` works.

## Building

Requirements: a C++20 compiler (GCC, Clang, or MSVC), CMake 3.25+, and Ninja. FFmpeg and SDL3 are not needed until Phases 2 and 6.

```sh
# Debug with AddressSanitizer + UndefinedBehaviorSanitizer, no GUI
cmake -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DVGLYPH_ENABLE_SANITIZERS=ON \
  -DVGLYPH_BUILD_GUI=OFF
cmake --build build-debug
ctest --test-dir build-debug --output-on-failure

./build-debug/vglyph --version
```

CMake options:

| Option | Default | Description |
| :--- | :--- | :--- |
| `VGLYPH_BUILD_GUI` | `OFF` | Build the `vglyph-gui` executable. The headless CLI and core always build without GUI libraries. |
| `VGLYPH_ENABLE_SANITIZERS` | `OFF` | Enable ASan + UBSan on GCC/Clang. |

On Windows 11, configure with the vcpkg toolchain file (`-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`); dependencies come from `vcpkg.json`.

## Project layout

| Path | Purpose |
| :--- | :--- |
| `include/vglyph/`, `src/core/` | `vglyph-core`: the UI-free conversion engine |
| `src/cli/` | `vglyph` command-line executable |
| `src/gui/` | `vglyph-gui` desktop executable (stub for now) |
| `tests/` | Single-header test framework and CTest suites |

## Documentation

- [`SPEC.md`](SPEC.md): technical specification and phased roadmap (single source of truth)
- [`CLAUDE.md`](CLAUDE.md): working rules for AI coding agents on this repository

## License

vglyph is licensed under the [GNU General Public License v3.0](LICENSE). Third-party components and their licenses will be listed in `THIRD_PARTY_LICENSES.md` once they are vendored.
