# vglyph — Technical Specification (`SPEC.md`)

**Status:** Draft v0.2 (Pre-Implementation Baseline, ambiguities resolved)  
**License:** GNU General Public License v3.0 (GPLv3)  
**Target Platforms:** Windows 11 (x86_64) and Linux (x86_64 / aarch64)  
**Language & Toolchain:** C++20 (`-fno-exceptions -fno-rtti`), CMake + Ninja + vcpkg

---

## 1. Project Overview, Goals, and Non-Goals

### 1.1 Overview
`vglyph` is a fast, minimal, cross-platform ASCII art converter and player for photos, animated GIFs, and videos. It provides a shared zero-UI C++20 engine (`vglyph-core`), a terminal-native CLI (`vglyph`) capable of TrueColor playback and file export, and a lightweight GPU-accelerated desktop GUI (`vglyph-gui`) built on SDL3 and Dear ImGui.

### 1.2 Core Goals
- **Unified Media Ingestion:** Decode still images (PNG, JPEG, WebP, BMP), animated GIFs, and videos (MP4, MKV, WebM, MOV, AVI) through a single FFmpeg (`libav*`) pipeline.
- **High-Fidelity Rendering:** Support classic ASCII luminance ramps, Unicode Half-Block (`▀`/`▄`) double-vertical-resolution rendering, and 24-bit TrueColor foreground/background mapping, with a clean extension seam for Sobel edge detection, Braille (`2x4`), and dithering.
- **Flexible Export:** Export rendered output both as text/terminal streams (`.txt`, `.ans`) and as rasterized visual media (`.png`, `.gif`, `.mp4`) using an embedded monospace font or a user-supplied `.ttf`.
- **Lean Engineering:** No C++ exceptions, no RTTI, strict RAII around C APIs, bounded streaming memory usage, and zero heavy framework bloat.
- **Native Cross-Platform Parity:** First-class support for both Linux terminals/desktops and Windows 11 (Windows Terminal + Win32 desktop).

### 1.3 Non-Goals (Explicitly Out of Scope for v1.0)
- **Audio Playback and Audio Muxing:** v1.0 is video/visual-only. Audio streams are skipped during decoding while preserving stream index metadata hooks for future passthrough and A/V sync.
- **Live Camera / Screen Capture:** No webcam (`v4l2`/`DirectShow`) or desktop capture ingestion in v1.0.
- **Network / URL Streaming:** Local filesystem inputs only; no HTTP/RTSP/HLS ingestion.
- **Plugin / Scripting API:** No Lua/Python scripting or dynamic plugin loader.
- **GPU Compute Shaders:** ASCII mapping runs on CPU in `vglyph-core` so CLI and GUI produce bit-identical output.

---

## 2. High-Level Architecture and Module Boundaries

### 2.1 Binary & Library Topology
The workspace compiles into one static library and two independent executables:

| Target | Type | Responsibilities | Allowed Dependencies |
| :--- | :--- | :--- | :--- |
| **`vglyph-core`** | Static Library | Data models, conversion pipeline, filters, FFmpeg reader/exporter, `stb_truetype` rasterizer, `.txt`/`.ans` serializer | C++20 STL, FFmpeg (`libavformat`, `libavcodec`, `libswscale`, `libavutil`), `stb_truetype`, embedded OFL `.ttf` |
| **`vglyph`** | Executable (CLI) | Argument parsing, `convert` / `play` / `info` commands, terminal playback loop, OS console RAII adapter | `vglyph-core`, minimal OS console APIs (`termios`/signals on Linux; Win32 Console API on Windows 11) |
| **`vglyph-gui`** | Executable (GUI) | Desktop window, live preview texture, parameter controls, timeline scrubbing, drag-and-drop, non-blocking export | `vglyph-core`, `SDL3`, `Dear ImGui` |

**Hard Boundary Rule:** `vglyph-core` MUST have zero knowledge of terminals, windows, SDL3, Dear ImGui, or OS-specific headers. Building with `-DVGLYPH_BUILD_GUI=OFF` MUST succeed on headless Linux with no GUI libraries installed.

### 2.2 Proposed Repository Layout
- `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`
- `LICENSE` (GPLv3), `THIRD_PARTY_LICENSES.md`, `SPEC.md`, `CLAUDE.md`, `README.md`, `CHANGELOG.md`
- `assets/fonts/` — Embedded OFL monospace `.ttf` font (compiled into a read-only byte array header at build time)
- `third_party/` — Vendored `stb_truetype.h` (single header)
- `include/vglyph/` — Public headers of `vglyph-core`
- `src/core/` — Error types, `Cell`/`AsciiFrame`, luminance/preprocessing, ASCII & Half-Block mappers, Phase 5 filters
- `src/media/` — FFmpeg RAII wrappers, `MediaReader`, `MediaExporter` (`.png`, `.gif`, `.mp4`)
- `src/render/` — Text/ANSI serializer, `stb_truetype` glyph atlas & RGBA rasterizer
- `src/cli/` — CLI parser, commands, terminal player, `platform_posix.cpp` / `platform_win32.cpp`
- `src/gui/` — `SDL3` + `Dear ImGui` application, background worker thread, viewport & inspector panels
- `tests/` — `test_framework.hpp` and unit/integration test suites

---

## 3. Core Data Model

All fallible APIs return `[[nodiscard]] Result<T, Error>` (backed by `std::variant`, accessed strictly via `std::get_if`) or `[[nodiscard]] Status`.

### 3.1 Error Model
| Field | Type | Description |
| :--- | :--- | :--- |
| `code` | `ErrorCode` enum | Categorized machine-readable error (`Ok`, `InvalidArgument`, `IoError`, `UnsupportedFormat`, `DecodeError`, `EncodeError`, `FontError`, `LimitExceeded`, `Cancelled`, `InternalError`) |
| `detail` | `std::string` | Concise, English diagnostic message (including mapped FFmpeg `av_strerror` text when relevant) |

Size and overflow violations are reported as `LimitExceeded`. There is no `OutOfMemory` code: because exceptions are disabled, a failed heap allocation aborts the process, and the safety caps in Section 5.3 exist to keep allocations bounded.

### 3.2 Pixel and Grid Primitives
- **`Rgb`**: 3-byte tightly packed color (`uint8_t r, g, b`).
- **`Cell`**: Single grid cell (`char32_t codepoint`, `Rgb fg`, `Rgb bg`). Fixed-size POD-like struct enabling contiguous cache-friendly storage.
- **`AsciiFrame`**:
  | Field | Type | Description |
  | :--- | :--- | :--- |
  | `cols` | `uint32_t` | Grid width in cells |
  | `rows` | `uint32_t` | Grid height in cells |
  | `cells` | `std::vector<Cell>` | Row-major contiguous array of length `cols * rows` |
  | `pts_us` | `int64_t` | Presentation timestamp in microseconds (0 for still images) |
  | `duration_us` | `int64_t` | Frame display duration in microseconds (0 for still images) |

The converter never returns an `AsciiFrame` by value. It writes into a caller-owned frame (`Status convert(..., AsciiFrame& out)`): `out.cells` is resized only when the grid size changes, so a playback loop reuses the same buffer and performs no per-frame allocation.

### 3.3 `ConvertConfig`
| Parameter | Type / Range | Default | Description |
| :--- | :--- | :--- | :--- |
| `target_cols` | `uint32_t` (`4..2048`) | `100` | Desired output width in character cells |
| `target_rows` | `uint32_t` (`0` = auto) | `0` | Explicit height; `0` derives height from source aspect ratio and `cell_aspect` |
| `cell_aspect` | `float` (`0.1..4.0`) | `0.5` | Character cell width-to-height ratio (terminal default `0.5`; auto-derived from font metrics during visual export) |
| `render_mode` | enum | `Ascii` | `Ascii`, `HalfBlock` (v1); `Braille`, `SobelAscii` (Phase 5) |
| `color_mode` | enum | `TrueColorFg` | `Mono`, `Grayscale`, `TrueColorFg`, `TrueColorFgBg` |
| `dither_mode` | enum | `None` | `None` (v1); `Bayer4x4`, `FloydSteinberg` (Phase 5) |
| `charset` | UTF-32 ramp (dark to bright) | `" .:-=+*#%@"` | Customizable ordered character ramp (at least 2 codepoints) |
| `bg_color` | `Rgb` | `(0, 0, 0)` | Default background color when `color_mode` does not set per-cell background |
| `brightness` | `float` (`-1.0..1.0`) | `0.0` | Linear additive brightness adjustment |
| `contrast` | `float` (`0.0..4.0`) | `1.0` | Contrast multiplier centered around mid-gray (`0.5`) |
| `gamma` | `float` (`0.1..4.0`) | `1.0` | Gamma exponent applied to normalized channel values |
| `invert` | `bool` | `false` | Inverts luminance mapping (useful for light-background terminals) |

### 3.4 `MediaInfo`
Returned by `MediaReader::info()` and printed by `vglyph info`:
- `media_kind`: `Image`, `AnimatedImage` (GIF/APNG/WebP), or `Video`
- `src_width`, `src_height`: Original frame dimensions in pixels
- `pixel_format_name`: Human-readable FFmpeg pixel format string
- `codec_name`: Decoded stream codec name
- `avg_fps`: Estimated average frame rate (`0.0` for still images)
- `duration_us`: Total estimated duration in microseconds (`0` if unknown or still)
- `has_audio`: Boolean indicator (stored along with audio stream index as a v2 passthrough hook)

---

## 4. Conversion Pipeline

Every decoded frame enters `vglyph-core` as a tightly packed 24-bit `RGB24` buffer scaled directly to the converter's sampling grid.

### 4.1 Grid Sizing and Aspect Ratio Math
Given source image dimensions $W_s \times H_s$, requested `target_cols` ($C$), and `cell_aspect` ($\alpha = \text{cell\_width} / \text{cell\_height}$):
- When `target_rows == 0`:
  $$R = \max\!\left(1,\ \operatorname{round}\!\left(C \cdot \frac{H_s}{W_s} \cdot \alpha\right)\right)$$
  If the computed `R` exceeds 1024, conversion fails with `LimitExceeded`; the grid is never silently cropped or shrunk. The caller lowers `target_cols` or sets `target_rows` explicitly.
- **Sampling Grid Resolution:**
  - `Ascii` / `SobelAscii` modes: $C \times R$ pixels.
  - `HalfBlock` mode: $C \times (2R)$ pixels (top and bottom sub-pixels per cell).
  - `Braille` mode (Phase 5): $(2C) \times (4R)$ pixels ($2 \times 4$ dot matrix per cell).
- Downscaling is performed via `libswscale` using `SWS_AREA` (area averaging), which prevents aliasing shimmer during video playback compared to nearest-neighbor or bilinear sampling.

### 4.2 Per-Pixel Preprocessing
For each channel $c \in \{R, G, B\}$ normalized to $[0, 1]$:
1. Gamma correction: $c_1 = c^{\gamma}$
2. Contrast & brightness: $c_2 = \operatorname{clamp}((c_1 - 0.5) \cdot \text{contrast} + 0.5 + \text{brightness},\ 0,\ 1)$

Perceptual luminance uses BT.709 coefficients:
$$Y = 0.2126 \cdot R_2 + 0.7152 \cdot G_2 + 0.0722 \cdot B_2$$
If `invert == true`, luminance becomes $Y' = 1 - Y$.

### 4.3 Rendering Modes
- **Standard ASCII (`Ascii`, v1):** Maps $Y'$ linearly onto the active `charset` of length $N$:
  $$\text{idx} = \min\!\left(N - 1,\ \left\lfloor Y' \cdot N \right\rfloor\right)$$
  - `Mono`: `fg = (255, 255, 255)`, `bg = config.bg_color`.
  - `Grayscale`: `fg = (g, g, g)` where $g = \operatorname{round}(255 \cdot Y')$, `bg = config.bg_color`.
  - `TrueColorFg`: `fg = (R_2, G_2, B_2)` scaled to `0..255`, `bg = config.bg_color`.
  - `TrueColorFgBg`: `fg = (R_2, G_2, B_2)` scaled to `0..255`; `bg` is the same color darkened by a fixed factor, `bg_c = round(0.25 * fg_c)` per channel.
- **Unicode Half-Block (`HalfBlock`, v1):** Each cell consumes two vertically stacked pixels (top $p_t$, bottom $p_b$) and sets `codepoint = U+2580` (`▀`). Doubles effective vertical resolution in TrueColor terminals and visual exports. The color mode decides how `fg` (top half) and `bg` (bottom half) are derived:
  - `TrueColorFg` and `TrueColorFgBg`: `fg = p_t`, `bg = p_b` (the `0.25` darkening of `TrueColorFgBg` does not apply).
  - `Grayscale`: `fg = (g_t, g_t, g_t)` and `bg = (g_b, g_b, g_b)` with $g = \operatorname{round}(255 \cdot Y')$ of the respective pixel.
  - `Mono`: each half is thresholded at $Y' \ge 0.5$; a set half uses `(255, 255, 255)`, an unset half uses `config.bg_color`. `fg` carries the top half and `bg` the bottom half.
- **Modular Phase 5 Extensions (Same `Cell` Output Contract):**
  - **Sobel Edge-Aware ASCII (`SobelAscii`):** Computes $3 \times 3$ Sobel gradients $(G_x, G_y)$ on the luminance plane; where magnitude exceeds threshold, replaces the ramp glyph with directional strokes (`|`, `-`, `/`, `\`) based on gradient angle $\theta = \operatorname{atan2}(G_y, G_x)$.
  - **Braille (`Braille`):** Thresholds each $2 \times 4$ pixel block (with optional dithering) into Unicode Braille range `U+2800..U+28FF`, assigning the block's mean color to `fg`.
  - **Dithering (`Bayer4x4` / `FloydSteinberg`):** Applied to luminance prior to ramp quantization or Braille thresholding.

---

## 5. Media Decoding Layer (FFmpeg `MediaReader`)

### 5.1 Single-Door Pipeline
1. Open input with `avformat_open_input` and probe streams with `avformat_find_stream_info`.
2. Locate best video/image stream (`av_find_best_stream(AVMEDIA_TYPE_VIDEO)`). Optionally record best audio stream index in `MediaInfo` without opening an audio decoder in v1.
3. Open matched decoder (`avcodec_find_decoder`, `avcodec_open2`) with multi-threaded decoding enabled where supported for video streams.
4. Allocate reusable `AVPacket`, decoded `AVFrame`, and scaled `RGB24` target buffer. Create/recreate `SwsContext` lazily only when source format or target grid dimensions change.

### 5.2 Frame Timing & Seeking
- Convert packet/frame `pts` and `duration` from stream `time_base` to microseconds using `av_rescale_q`.
- If a frame lacks a valid duration (common in some MP4/WebM files), fall back to the delta between consecutive PTS values or `1 / avg_fps`; for single-frame images, `pts_us = 0`, `duration_us = 0`.
- For animated GIFs with zero-delay frames (legacy GIF quirk), clamp delays of 10ms or less to a sensible default (`100ms` / 10 FPS), exactly as browsers do (FFmpeg itself only replaces a 0cs delay, so the reader clamps the rest and builds the GIF timeline from the clamped delays).
- `seek_us(int64_t target_us)` performs keyframe seek (`av_seek_frame` with `AVSEEK_FLAG_BACKWARD`), flushes codec buffers (`avcodec_flush_buffers`), and decodes forward cleanly to the requested timestamp for GUI scrubbing.

### 5.3 Input Safety Limits
To prevent memory exhaustion and integer overflow on malformed or adversarial inputs:
- Reject source dimensions `<= 0` or `> 16384` on either axis (`LimitExceeded`).
- Reject total source pixel count `> 67,108,864` (64 megapixels).
- Reject output grid sizes exceeding `cols > 2048` or `rows > 1024`, including an automatically derived `rows` (Section 4.1).
- All buffer byte-size calculations (`width * height * channels`) MUST use checked 64-bit multiplication before allocation.
- A frame's `pts_us` and `duration_us` MUST lie within `±2^51` microseconds (about 71 years); timestamps outside that, or that cannot be computed without overflow, are a `DecodeError`. Arithmetic on stream timestamps and seek targets is overflow-checked.

---

## 6. Text Serialization and Terminal Playback

### 6.1 Text & ANSI Export
- **`.txt`:** Plain UTF-8 encoded characters with `\n` line endings (strips color metadata).
- **`.ans`:** UTF-8 text enriched with SGR 24-bit TrueColor escape sequences (`ESC[38;2;R;G;Bm` for `fg`, `ESC[48;2;R;G;Bm` for `bg`, `ESC[0m` reset at the end of every row, so a background color never bleeds across a line break or when the terminal scrolls). Rows are separated by `\n`; the last row of a frame has no line break, and file writers append the final `\n`.
- **Run-Length Color Deduplication:** Every row starts by writing both `fg` and `bg`; after that, emit `fg` or `bg` escape sequences **only** when a cell's color differs from the immediately preceding cell on the same row. This dramatically shrinks `.ans` files and reduces terminal I/O overhead during 60 FPS playback.
- **TrueColor Only:** `.ans` output and terminal playback always use 24-bit SGR sequences. There is no 256-color fallback and no terminal capability detection in v1; a TrueColor-capable terminal is required.

### 6.2 Terminal Playback Engine (`vglyph play`)
- **Single-Write Frame Flushing:** Format the entire frame into one reusable output byte buffer (prefixed with cursor-home `ESC[H]` and wrapped in Synchronized Output `ESC[?2026h` ... `ESC[?2026l`), then emit with a single write call to eliminate tearing and flicker.
- **PTS-Paced Clock:** Track playback via `std::chrono::steady_clock`. Sleep until the next frame's `pts_us`; if decoding/rendering falls behind by more than one frame duration, skip rendering late frames while continuing decode to maintain real-time speed.
- **Fit to Terminal:** The grid is fitted into the terminal's `cols x rows` while preserving aspect ratio (in `HalfBlock` mode the available height is `2 * rows` pixels). Explicit `-w` / `-H` values are also clamped to the terminal size. On `SIGWINCH` (or the Windows console resize event) the grid is recomputed.
- **Still Images:** `play` on a photo draws the single frame once, then waits until the user quits with `q` / `Esc`.
- **Interactive Controls:** Non-blocking keyboard polling during playback for `Space` (pause/resume), `q` / `Esc` (quit), and `l` (toggle loop).
- **Platform Adapters:**
  - **Linux (`platform_posix.cpp`):** Query window size via `ioctl(TIOCGWINSZ)`, handle `SIGWINCH` for dynamic resize, switch `termios` to non-canonical/no-echo mode, restore state on exit or `SIGINT`/`SIGTERM`.
  - **Windows 11 (`platform_win32.cpp`):** Enable `ENABLE_VIRTUAL_TERMINAL_PROCESSING` on `stdout`, set console output code page to `CP_UTF8`, query console size via `GetConsoleScreenBufferInfo`, poll input via `PeekConsoleInput`/`_kbhit`, and restore state via RAII + `SetConsoleCtrlHandler`.

---

## 7. Font Rasterizer and Visual/Video Export

### 7.1 Glyph Atlas Rasterizer (`stb_truetype`)
- **Default Font:** *JetBrains Mono* Regular (SIL OFL), embedded directly into `vglyph-core` so visual export works out of the box with zero external asset lookup.
- **Custom Font Support:** Users may supply a local TrueType (`.ttf`) path and a target font pixel height (`8..64 px`, default `16 px`). CFF-based `.otf` files are not supported in v1; if `stb_truetype` cannot load the file, initialization fails with `FontError`.
- **Atlas Caching:** On font/size initialization, query exact monospace metrics (`advance_width`, `ascent`, `descent`, `line_gap`) to determine uniform `cell_px_w` and `cell_px_h`. Pre-rasterize all glyphs in the active ramp into an 8-bit coverage atlas. If any codepoint of the active charset has no glyph in the font, atlas creation fails with `FontError` and the missing codepoint is named in `detail`; there is no fallback glyph.
- **Frame Rasterization (`AsciiFrame` → `RGBA32`):**
  - Fill cell background with `cell.bg`.
  - For `HalfBlock` (`▀`), fill top half with `cell.fg` and bottom half with `cell.bg` directly (pixel-sharp, bypassing font hinting).
  - For character glyphs, alpha-blend the cached 8-bit glyph bitmap over `cell.bg` using `cell.fg`.
  - Output pixel dimensions: `out_w = cols * cell_px_w`, `out_h = rows * cell_px_h`.

### 7.2 Visual & Video Exporters (FFmpeg `libavcodec` / `libavformat`)
| Output Format | Applicable Inputs | Encoding Strategy |
| :--- | :--- | :--- |
| **`.png`** | Photo (or single selected frame of GIF/video) | FFmpeg `AV_CODEC_ID_PNG` (`AV_PIX_FMT_RGBA`) |
| **`.gif`** | Animated GIF or video | Two-pass or streaming 256-color palette quantization (`AV_CODEC_ID_GIF`) preserving original frame delays (clamped to GIF centisecond resolution `1/100s`); output loops forever (NETSCAPE loop count `0`) |
| **`.mp4`** | Animated GIF or video | H.264 via `libx264` (`AV_PIX_FMT_YUV420P`, configurable CRF default `20`, `+faststart`). Automatically pads odd `out_w` or `out_h` by 1 pixel with `config.bg_color` to satisfy `yuv420p` even-dimension requirement |

---

## 8. CLI Specification (`vglyph`)

### 8.1 Subcommands
- `vglyph convert <input> -o <output> [options]` — Convert photo, GIF, or video to `.txt`, `.ans`, `.png`, `.gif`, or `.mp4` (inferred from `<output>` extension or overridden via `--format`). `-o -` writes to `stdout` instead of a file; without `--format` it writes `.ans`, so use `--format txt` for plain text.
- `vglyph play <input> [options]` — Play photo, GIF, or video directly in the terminal with TrueColor ANSI rendering.
- `vglyph info <input>` — Print structured media metadata (`MediaInfo`) to `stdout`, one `key: value` line per field in this order: `kind`, `width`, `height`, `pixel_format`, `codec`, `fps`, `duration_us`, `has_audio`.
- `vglyph --version` / `vglyph --help` — Print version/GPLv3 notice or usage help.

### 8.2 Core Flags
| Flag | Values / Default | Scope | Description |
| :--- | :--- | :--- | :--- |
| `-w, --width <cols>` | `4..2048` (fitted to the terminal in `play`, `100` in `convert`) | `convert`, `play` | Output grid width in cells |
| `-H, --height <rows>` | `0` (auto) | `convert`, `play` | Explicit grid height |
| `--aspect <float>` | `0.5` | `convert`, `play` | Override cell aspect ratio |
| `--mode <mode>` | `ascii`, `halfblock`, `braille`, `sobel` | `convert`, `play` | Rendering mode (`braille` and `sobel` are parsed but exit `2` with a "not implemented" error until Phase 5) |
| `--color <mode>` | `mono`, `gray`, `fg`, `fgbg` | `convert`, `play` | Color mapping mode |
| `--charset <string>` | `" .:-=+*#%@"` | `convert`, `play` | Custom dark-to-bright glyph ramp |
| `--dither <mode>` | `none`, `bayer`, `floyd` | `convert`, `play` | Dithering algorithm (`bayer` and `floyd` are parsed but exit `2` with a "not implemented" error until Phase 5) |
| `--brightness <float>` | `-1.0..1.0` (`0.0`) | `convert`, `play` | Brightness offset |
| `--contrast <float>` | `0.0..4.0` (`1.0`) | `convert`, `play` | Contrast factor |
| `--gamma <float>` | `0.1..4.0` (`1.0`) | `convert`, `play` | Gamma curve |
| `--invert` | flag | `convert`, `play` | Invert luminance ramp |
| `--font <path>` | embedded default | `convert` | Custom TrueType (`.ttf`) font for visual export |
| `--font-size <px>` | `8..64` (`16`) | `convert` | Font height in pixels for visual export |
| `--crf <int>` | `0..51` (`20`) | `convert` (`.mp4`) | H.264 quality factor |
| `--loop` | flag | `play` | Loop GIF/video playback indefinitely |
| `--frame <index>` | `0` | `convert` | Extract a single frame (0-based, in decode order) from GIF/video when exporting `.txt`/`.ans`/`.png`; an index past the last frame is `InvalidArgument` (exit `2`) |

### 8.3 Stream & Exit Code Contract
- **Streams:** Primary converted text output (`-o -`) and `info` go to `stdout`. Progress bars, warnings, and errors always go to `stderr`.
- **Exit Codes:** `0` Success, `1` Runtime / I/O / Decode / Encode error, `2` Invalid CLI usage or arguments, `130` Interrupted by user (`SIGINT`).

---

## 9. Desktop GUI Specification (`vglyph-gui`)

Built with **`SDL3`** (`SDL_Renderer`, hardware-accelerated 2D texture presentation) and **`Dear ImGui`** (dockable/clean dark studio layout).

### 9.1 Window Layout
- **Center Viewport:** Zoomable/pannable live preview rendered to a streaming `SDL_Texture` backed by `vglyph-core`'s `stb_truetype` RGBA rasterizer (ensuring WYSIWYG parity between preview and `.png`/`.gif`/`.mp4` export). Supports drag-and-drop file loading (`SDL_EVENT_DROP_FILE`).
- **Right Inspector Panel:** Live controls bound directly to `ConvertConfig` (width slider, mode/color selectors, preset & custom charset input, brightness/contrast/gamma sliders, invert toggle, font picker & font size).
- **Bottom Transport Bar (GIF/Video):** Play/Pause button, timeline scrubber (triggers `seek_us`), current/total timestamp readout, loop toggle, and playback FPS indicator.
- **Top Menu / Action Bar:** Open File (`SDL_ShowOpenFileDialog`), Export dialog (`.txt`, `.ans`, `.png`, `.gif`, `.mp4`), and quick Copy-as-Text action.

### 9.2 Threading & Responsiveness
- **Main Thread:** Exclusively handles SDL3 event loop, ImGui UI rendering, and uploading the latest completed RGBA frame to `SDL_Texture`. Never runs FFmpeg decoding or exporting.
- **Playback Worker Thread:** Runs `MediaReader` + `Converter` + `Rasterizer`, communicating via lock-free/mutex-protected double-buffered frame swap and atomic config version counter. Parameter changes while paused immediately re-render the current frame.
- **Export Worker Thread:** Runs full-file conversion/encoding asynchronously with an atomic progress fraction (`0.0..1.0`) and cancellation flag (`ErrorCode::Cancelled`), keeping the UI responsive throughout long video exports.

---

## 10. Error Handling, Resource Safety, and Performance Targets

- **Deterministic Cleanup:** All FFmpeg contexts, frames, packets, `SwsContext`, file streams, console state, and SDL3 handles are owned exclusively by RAII wrappers.
- **Zero-Allocation Hot Path:** After initial setup (or config/resolution change), converting, rasterizing, and flushing frames in a loop MUST perform zero heap allocations. This is why the converter writes into a caller-owned `AsciiFrame` (Section 3.2).
- **Performance Targets (Reference Machine: Mid-range x86_64 Desktop CPU):**
  - **1080p video → 160-column ASCII + TrueColor:** $\ge 60\text{ FPS}$ end-to-end (decode + scale + map + rasterize/format).
  - **Still 4K image conversion:** $< 50\text{ ms}$.
  - **Steady-State RAM Ceiling (1080p video playback):** $< 150\text{ MB}$ RSS (no frame accumulation over time).

---

## 11. Test Matrix and Phased Implementation Roadmap

### 11.1 Test Strategy
- **Framework:** Custom single-file `tests/test_framework.hpp` integrated with `CTest`. No external test frameworks.
- **Determinism:** Unit tests construct synthetic gradients, checkerboards, and edge patterns directly in memory. Integration tests generate tiny synthetic PNG/GIF/MP4 files in a temporary directory via `MediaExporter`, then read them back via `MediaReader` (round-trip verification).
- **CI Matrix:**
  - Linux GCC & Clang: Debug (`ASan + UBSan + -fno-sanitize-recover=all + -Werror`) and Release, both with `VGLYPH_BUILD_GUI=OFF` and `ON`.
  - Windows 11 MSVC: Debug and Release (`/W4 /WX`, Ninja + vcpkg).
  - CI starts in Phase 0 (GitHub Actions). Until Phase 2 the CI jobs run without vcpkg because FFmpeg is an optional `media` vcpkg feature; Phase 2 enables it together with a binary cache.

### 11.2 Phased Roadmap

| Phase | Scope & Deliverables | Exit Criteria |
| :--- | :--- | :--- |
| **Phase 0: Project Skeleton & Build Gate** | CMake + Ninja + `vcpkg.json`, `LICENSE` (GPLv3), `vglyph-core` / `vglyph` / `vglyph-gui` stub targets (`VGLYPH_BUILD_GUI` toggle), `tests/test_framework.hpp`, compiler flags & sanitizer wiring, GitHub Actions CI (Linux GCC/Clang, Windows MSVC) | Clean zero-warning build and passing smoke test on Linux (ASan/UBSan) and Windows 11 (MSVC), verified green in CI |
| **Phase 1: Error Model & Core Conversion Engine** | `Result`/`Status`, `Rgb`/`Cell`/`AsciiFrame`, `ConvertConfig`, aspect ratio sizing, gamma/contrast/brightness, BT.709 luminance, `Ascii` and `HalfBlock` mappers, all 4 color modes | 100% deterministic unit test pass on synthetic patterns, bounds/overflow tests, zero sanitizer findings |
| **Phase 2: FFmpeg Media Reader (`MediaReader`)** | `libav*` RAII wrappers, photo/GIF/video ingestion, `libswscale` (`SWS_AREA`), VFR `pts_us`/`duration_us`, seeking, safety dimension caps, malformed-file error handling | Reads synthetic PNG, GIF, and MP4 streams cleanly; zero FFmpeg leaks under ASan/LSan |
| **Phase 3: Text Serialization & `vglyph` CLI (`convert` + `play` + `info`)** | `.txt`/`.ans` serializer with run-length color deduplication, CLI parser, `platform_posix.cpp` & `platform_win32.cpp` RAII guards, DEC 2026 single-write PTS-paced terminal player | Smooth flicker-free playback on Linux and Windows Terminal; terminal cleanly restored on `Ctrl+C`; unit tests for ANSI compression & parser |
| **Phase 4: Glyph Rasterizer & Visual/Video Export** | Vendored `stb_truetype.h`, embedded OFL `.ttf` font, glyph atlas cache, RGBA rasterizer, FFmpeg `.png`/`.gif`/`.mp4` (`libx264`, even-dimension alignment) exporters, CLI export flags | Round-trip export → decode tests pass for `.png`, `.gif`, and `.mp4` (including odd-dimension alignment); custom `.ttf` test passes |
| **Phase 5: Advanced Modular Filters** | `SobelAscii` edge-aware mapper, `Braille` (`2x4`) mode, `Bayer4x4` and `FloydSteinberg` dithering integrated into core and CLI flags | Deterministic edge/pattern/dithering unit tests pass with zero hot-path allocations |
| **Phase 6: Desktop GUI (`vglyph-gui`)** | `SDL3` + `Dear ImGui` shell, drag-and-drop + `SDL_ShowOpenFileDialog`, worker-threaded live preview & seek, inspector panel, non-blocking export dialog with progress & cancel | Smooth live preview on both Linux and Windows 11; clean shutdown with zero SDL3/worker race conditions |
| **Phase 7: Packaging, Profiling & v1.0 Release** | CI/CD release artifacts (Linux + Windows 11), performance benchmark & memory audit against Section 10 targets, complete `README.md`, `THIRD_PARTY_LICENSES.md`, `CHANGELOG.md` | All CI checks green, 1080p playback meets performance & RAM targets, ready for `v1.0.0` tag |