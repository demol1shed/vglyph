# vglyph

A fast, minimal, cross-platform ASCII art converter and player for photos, animated GIFs, and videos.

> **Status: pre-alpha.** The conversion engine, the FFmpeg media reader, the glyph rasterizer and the `vglyph` CLI (`convert` to `.txt`/`.ans`/`.png`/`.gif`/`.mp4`, `play`, `info`) work. Braille, Sobel, dithering and the GUI are specified in [`SPEC.md`](SPEC.md) but not implemented yet.

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
vglyph convert photo.jpg -o photo.png --font-size 24
vglyph convert clip.mp4  -o ascii.mp4 --mode halfblock --crf 18
vglyph convert anim.gif  -o ascii.gif --font /path/to/mono.ttf
vglyph play    clip.mp4  --loop
vglyph info    clip.mp4
```

- `convert` picks the format from the output's extension (or `--format`):
  - `.txt`, `.ans` (24-bit color) and `.png` hold one frame (`--frame N`). `-o -` writes text to stdout, as `.ans` unless `--format txt` is given.
  - `.gif` and `.mp4` hold the whole animation or video, with its frame times; they need an animated or video input. Audio is not copied.
  - `.png`, `.gif` and `.mp4` are drawn with the embedded JetBrains Mono, or a TrueType font given with `--font`, at `--font-size` (8..64, default 16). `--crf` sets MP4 quality. The file is written to `<output>.vglyph-tmp` first and renamed when complete, so a failed or interrupted export leaves an existing output untouched.
- `play` draws photos, GIFs and videos in the terminal, paced by the frames' timestamps. `Space` pauses, `l` toggles looping, `q` or `Esc` quits; resizing the terminal refits the picture. It needs an interactive terminal. Audio is not played.
- `info` prints `key: value` lines of the file's metadata.

Run `vglyph --help` for every option. Exit codes: `0` success, `1` runtime error, `2` bad usage (including `--frame` past the end), `130` interrupted.

Not available yet: `--mode braille|sobel` and `--dither bayer|floyd` (Phase 5).

Known limits: pass `--font` only fonts you trust, because the font parser (`stb_truetype`) does not validate the offsets inside a font file and a crafted one can crash vglyph or make it read memory it should not. Interactive `play` has been run on Linux only; the Windows console code is compiled and smoke-tested in CI but not yet tried by hand. On Linux, arrow keys quit the player (they start with `Esc`). Interrupting an export has likewise been tried on Linux only. If an export is killed and leaves `<output>.vglyph-tmp` behind, delete it before exporting to the same file again.

## Building

Requirements: a C++20 compiler (GCC, Clang, or MSVC), CMake 3.25+, Ninja, and FFmpeg development libraries (`libavformat`, `libavcodec`, `libswscale`, `libavutil`; found through `pkg-config` on Linux, `vcpkg` on Windows; MP4 export needs an FFmpeg built with `libx264`). SDL3 is not needed until Phase 6.

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
| `src/render/` | `.txt` / `.ans` serialization, glyph atlas and RGBA rasterizer |
| `src/media/` | FFmpeg-backed media reader and PNG/GIF/MP4 exporter |
| `src/cli/` | `vglyph` command-line executable: parser, commands, terminal player, platform adapters |
| `src/gui/` | `vglyph-gui` desktop executable (stub for now) |
| `tests/` | Single-header test framework and CTest suites |
| `assets/fonts/`, `third_party/` | Embedded font and vendored `stb_truetype.h` |

## Documentation

- [`SPEC.md`](SPEC.md): technical specification and phased roadmap (single source of truth)
- [`CLAUDE.md`](CLAUDE.md): working rules for AI coding agents on this repository

## License

vglyph is licensed under the [GNU General Public License v3.0](LICENSE). Third-party components and their licenses are listed in [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
