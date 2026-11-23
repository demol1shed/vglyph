# vglyph

A fast, minimal, cross-platform ASCII art converter and player for photos, animated GIFs, and videos.

> **Status: pre-alpha.** The conversion engine, the FFmpeg media reader and the `vglyph` CLI (`convert` to `.txt`/`.ans`, `play`, `info`) work. PNG/GIF/MP4 export, the glyph rasterizer and the GUI are specified in [`SPEC.md`](SPEC.md) but not implemented yet.

## Planned features

- **One decoding path:** PNG, JPEG, WebP, BMP, animated GIF, and common video containers (MP4, MKV, WebM, MOV, AVI) all go through FFmpeg.
- **Rendering modes:** classic luminance ramps, Unicode half-blocks (`▀`) for double vertical resolution, and 24-bit TrueColor foreground/background. Braille, Sobel edge detection, and dithering follow later.
- **Terminal playback:** flicker-free, PTS-paced playback with `vglyph play`.
- **Export:** text and ANSI (`.txt`, `.ans`), and rasterized `.png`, `.gif`, `.mp4` using an embedded OFL monospace font or your own `.ttf`.
- **Desktop GUI:** a lightweight SDL3 + Dear ImGui front end with live preview and non-blocking export.
- **Lean engineering:** C++20 without exceptions or RTTI, RAII around every C API, bounded streaming memory, and bit-identical output between CLI and GUI.

Platforms: Linux (x86_64 / aarch64) and Windows 11 (x86_64). A TrueColor (24-bit) terminal is required for `vglyph play` and `.ans` output; there is no 256-color fallback.

## CLI

```sh
vglyph convert photo.jpg -o photo.ans -w 120 --mode halfblock
vglyph convert photo.jpg -o - --format txt | head
vglyph play    clip.mp4  --loop
vglyph info    clip.mp4
```

- `convert` writes one frame (`--frame N`) as `.txt` or `.ans` (24-bit color). `-o -` writes to stdout, as `.ans` unless `--format txt` is given.
- `play` draws photos, GIFs and videos in the terminal, paced by the frames' timestamps. `Space` pauses, `l` toggles looping, `q` or `Esc` quits; resizing the terminal refits the picture. It needs an interactive terminal. Audio is not played.
- `info` prints `key: value` lines of the file's metadata.

Run `vglyph --help` for every option. Exit codes: `0` success, `1` runtime error, `2` bad usage (including `--frame` past the end), `130` interrupted.

Not available yet: `.png`/`.gif`/`.mp4` export and `--font`, `--font-size`, `--crf` (Phase 4), `--mode braille|sobel` and `--dither bayer|floyd` (Phase 5).

Known limits: interactive `play` has been run on Linux only; the Windows console code is compiled and smoke-tested in CI but not yet tried by hand. On Linux, arrow keys quit the player (they start with `Esc`).

## Building

Requirements: a C++20 compiler (GCC, Clang, or MSVC), CMake 3.25+, Ninja, and FFmpeg development libraries (`libavformat`, `libavcodec`, `libswscale`, `libavutil`; found through `pkg-config` on Linux, `vcpkg` on Windows). SDL3 is not needed until Phase 6.

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
| `src/render/` | `.txt` / `.ans` serialization |
| `src/media/` | FFmpeg-backed media reader |
| `src/cli/` | `vglyph` command-line executable: parser, commands, terminal player, platform adapters |
| `src/gui/` | `vglyph-gui` desktop executable (stub for now) |
| `tests/` | Single-header test framework and CTest suites |

## Documentation

- [`SPEC.md`](SPEC.md): technical specification and phased roadmap (single source of truth)
- [`CLAUDE.md`](CLAUDE.md): working rules for AI coding agents on this repository

## License

vglyph is licensed under the [GNU General Public License v3.0](LICENSE). Third-party components and their licenses will be listed in `THIRD_PARTY_LICENSES.md` once they are vendored.
