This file defines the binding working rules for AI coding agents (Claude Code) working on **`vglyph`** (an open-source, GPLv3, cross-platform C++20 ASCII art application for Windows 11 and Linux supporting photos, GIFs, and videos).

The Single Source of Truth for technical requirements, system architecture, data models, the media/conversion pipeline, export formats, CLI/GUI contracts, and the phased test matrix is **`SPEC.md`** at the project root. Do not make any assumptions that contradict `SPEC.md`; if you encounter ambiguity, ask the user before writing code.

---

## 1. Role, Communication, and Workflow (Mandatory)

- **Division of Roles:** You implement as a senior C++20 systems and media developer; the user is the architect and code reviewer. A primary goal of this project is that every module remains clean, deterministic, and easy for a human to read and audit.
- **Language Rule:** For open-source hygiene, all source code, identifiers, comments, CLI/GUI help and error messages, test names, `SPEC.md`, `CLAUDE.md`, and `README.md` MUST be written in **English**.
- **Plan-First Approval Gate:**
  1. In any phase or task, **before writing any code**, you MUST present a brief implementation plan and the exact list of files to be created or modified.
  2. **Do not create files or modify code without explicit user approval.**
- **Single-Phase Rule:** Work only on the currently active phase (`SPEC.md` Section 11). Do not advance to the next phase until all build, warning, sanitizer, and test gates for the current phase pass with zero findings and the user approves.
- **Simplicity Rule:** Do not write convoluted template metaprogramming, speculative abstraction layers, or hard-to-read "clever" one-liners. Write explicit, modular, readable, and predictable C++20.

---

## 2. Strict Prohibitions (C++20 & Media Guardrails)

Violations of the following rules are strictly unacceptable:

1. **No C++ Exceptions and No RTTI (`-fno-exceptions -fno-rtti` / `/EHs-c- /GR- /D_HAS_EXCEPTIONS=0`):**
   - **NEVER** use `try`, `catch`, or `throw` anywhere in production code or tests.
   - All fallible functions MUST return `[[nodiscard]] vglyph::Result<T, Error>` or `[[nodiscard]] vglyph::Status`.
   - Avoid STL operations that throw on invalid input or bounds failure (e.g., `std::stoi`, `.at()`, `std::get`); use `std::from_chars`, explicit bounds checks, and `std::get_if`.
2. **No Core-to-UI or OS Coupling:**
   - The `vglyph-core` static library MUST **NEVER** include `SDL3`, `Dear ImGui`, Win32 (`<windows.h>`), or POSIX terminal (`<termios.h>`, `<sys/ioctl.h>`) headers.
   - All OS-specific console/signal code is strictly isolated inside dedicated CLI platform adapter files.
3. **No Raw Owning C Handles (FFmpeg & SDL3):**
   - **NEVER** manage raw owning pointers for `AVFormatContext`, `AVCodecContext`, `AVFrame`, `AVPacket`, `SwsContext`, `SDL_Window`, `SDL_Renderer`, or `SDL_Texture`.
   - Every C API resource MUST be wrapped in a non-copyable, move-only RAII type with deterministic cleanup.
4. **No Ignored Return Values:**
   - Return values of all FFmpeg (`libav*`) calls, `stb_truetype` initialization calls, file/console I/O calls, and `SDL3` calls MUST **NEVER** be ignored.
   - Every FFmpeg integer status MUST be explicitly checked for `< 0` (distinguishing `AVERROR(EAGAIN)` and `AVERROR_EOF` from true errors).
5. **No Hot-Path Per-Frame Allocations:**
   - Inside video playback, terminal rendering, and frame conversion loops, **NEVER** perform per-frame heap allocations (`new`, `malloc`, growing temporary `std::vector`/`std::string` buffers). Pre-allocate and reuse buffers across frames.
6. **No Unbounded Memory Growth or Unchecked Arithmetic:**
   - **NEVER** buffer an entire video stream in RAM. Process frames in a bounded streaming/pipelined manner.
   - Always validate decoded media dimensions, grid sizes, and byte-stride calculations with overflow-checked arithmetic against `SPEC.md` safety caps before allocating buffers.
7. **No Unapproved External Dependencies:**
   - Only dependencies explicitly allowlisted in `SPEC.md` may be used:
     - Core/Media: FFmpeg (`libavformat`, `libavcodec`, `libswscale`, `libavutil`), `stb_truetype` (vendored single header), and one embedded OFL monospace `.ttf` font.
     - GUI target (`vglyph-gui`) only: `SDL3` and `Dear ImGui`.
     - Tests: Our single-file `tests/test_framework.hpp` and `CTest`.
   - **NEVER** add third-party CLI parsers, JSON/YAML libraries, logging frameworks, Catch2, GoogleTest, OpenCV, or extra image libraries without explicit user approval.
8. **No Cross-Platform Path or Line-Ending Assumptions:**
   - Always use `std::filesystem::path` for filesystem paths and open binary media files explicitly in binary mode (`std::ios::binary`). Never hardcode `/` vs. `\` or assume UTF-8 console output on Windows 11 without explicit adapter initialization.

---

## 3. Architectural Implementation Nuances

- **Dual-Binary Layout:** `vglyph` (CLI) and `vglyph-gui` (desktop GUI) are separate executables linking the same static `vglyph-core`. A CMake option (`VGLYPH_BUILD_GUI=ON/OFF`) MUST allow building and testing `vglyph-core` and `vglyph` cleanly on headless Linux environments without SDL3/display dependencies.
- **Single-Door FFmpeg & VFR Timing:** Photos, animated GIFs, and videos all flow through `MediaReader` via `libav*`. Never assume constant frame rate; propagate per-frame `pts_us` and `duration_us` (microseconds) directly to the player and exporters. In v1, audio streams are skipped cleanly while preserving stream metadata hooks for future passthrough.
- **Aspect Ratio Compensation:** Terminal monospace cells are taller than they are wide (default `cell_aspect = 0.5`). For visual/video export, compute exact cell aspect ratio from the active font's metrics via `stb_truetype`. In Half-Block mode (`▀`/`▄`), scale vertically to `2 * grid_rows` pixels.
- **Glyph Atlas Caching:** Rasterize the active character ramp's glyphs into a single cached atlas via `stb_truetype` only when the font or pixel size changes—never per frame.
- **Even Dimensions for MP4 (`libx264` + `yuv420p`):** H.264 `yuv420p` encoding requires even pixel width and height. The MP4 exporter MUST automatically pad odd pixel dimensions cleanly (by at most 1 pixel with the configured background color) before color-space conversion.
- **Terminal Safety RAII Guard:** Before switching to alternate screen buffer, hiding the cursor, or configuring raw/VT mode (`termios` on Linux; `ENABLE_VIRTUAL_TERMINAL_PROCESSING` + `CP_UTF8` on Windows 11), install an RAII guard and interrupt handler (`SIGINT`/`SIGTERM` or `SetConsoleCtrlHandler`) that unconditionally restores terminal state on exit.
- **Licensing Hygiene:** The project is licensed under **GPLv3** (permitting full FFmpeg codec support including `libx264`). Any vendored font or single-header dependency must have a GPLv3-compatible license (e.g., SIL OFL, MIT, Public Domain) documented in `THIRD_PARTY_LICENSES.md`.

---

## 4. Build and Verification Gate (Definition of Done)

Before reporting any phase or change as "complete", **personally run** the applicable commands and summarize their outputs:

### Linux (GCC / Clang — Debug + ASan + UBSan + `-Werror`)

```bash
# 1. Configure Debug with ASan + UBSan
cmake -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DVGLYPH_ENABLE_SANITIZERS=ON \
  -DVGLYPH_BUILD_GUI=OFF

# 2. Build with zero warnings
cmake --build build-debug

# 3. Run all tests under sanitizers
ctest --test-dir build-debug --output-on-failure
```

Verify Release build (and GUI target when applicable):

```bash
cmake -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DVGLYPH_ENABLE_SANITIZERS=OFF \
  -DVGLYPH_BUILD_GUI=ON
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

### Windows 11 (MSVC + Ninja + vcpkg)

```powershell
cmake -B build-win-debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build-win-debug
ctest --test-dir build-win-debug --output-on-failure
```

**Acceptance Criteria:**
- Compiler warnings: **0** (`-Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow` on GCC/Clang; `/W4 /WX` on MSVC).
- ASan / UBSan findings: **0** (compiled with `-fno-sanitize-recover=all` so UBSan immediately fails CTest).
- Failing tests: **0**.

**Testing and Verification Rules:**
- Access `Result`/`Status` via `std::get_if` only; never call `std::get` (under `-fno-exceptions`, accessing the wrong alternative silently calls `std::abort`).
- Core tests MUST use synthetic in-memory pixel buffers and small generated fixtures—never commit large binary sample media files to Git.
- Every new behavior or bug fix requires corresponding unit/integration tests registered in `CMakeLists.txt`.

---

## 5. End-of-Phase Self-Audit Report

After coding and test runs for a phase are complete, present a concise **Self-Audit Summary** to the user:
1. Created/modified files and their brief responsibilities.
2. Resource and memory safety audit (FFmpeg/SDL3 RAII ownership, hot-path allocation check, buffer reuse and cleanup).
3. Boundary/overflow and error-path checks (dimension/stride overflow guards, FFmpeg `< 0` handling, malformed input paths).
4. Cross-platform (Windows 11 and Linux) compatibility notes and build/warning/sanitizer/CTest summary.

---

## 6. Phase Tracking

- **Current Phase:** Phase 1 (Error Model and Core Conversion Engine)
- **Completed Phases:** Phase 0 (Project Skeleton and Build Gate): verified locally and in CI (Linux GCC/Clang with ASan+UBSan, Windows MSVC).
- **Carry-over from Phase 0:**
  - `tests/test_framework.hpp` discards `std::printf` return values with `(void)`, which violates Section 2.4 (console I/O results must not be ignored). Fix it in the first Phase 1 PR: a failed write must make the test runner exit non-zero.
  - `vcpkg.json` feature names (`sdl3-binding`, `sdl3-renderer-binding`, the `ffmpeg` features) are unverified; confirm them when the `media` feature is enabled in Phase 2 and the `gui` feature in Phase 6.

---

## 7. Git Workflow

History is kept clean: small, descriptive commits at logical checkpoints; one commit = one logical change. Refactors, behavioral changes, and tests belong in separate commits so reviewers can read the code piece by piece. Commit messages and PR descriptions are written in **English**.

- **No** direct commits or pushes to `main`. Use a topic branch named `<type>/<short-description>` (e.g., `build/phase-0-skeleton`, `feat/core-ascii-mapper`) → push → `gh pr create` (Summary + Test plan) → merge via PR once CI passes → delete branch. One PR carries a single concern.
- **Merge strategy: rebase-merge** (preserves linear history and atomic commits). Exception: use squash-merge if the PR's commits are messy/WIP; the squash message must still follow the format below. Plain merge commits are never used.
- Ask the user before committing; push, tag, and release are outward-facing operations and are performed only when explicitly requested by the user.
- Every commit represents a single logical change, builds cleanly, and passes tests. Leave no WIP/fixup commits in a PR (use `--amend` before pushing).
- Never commit secrets, local environment files, large media fixtures, or build artifacts; avoid unrelated formatting changes.

### Commit Messages

Conventional Commits 1.0:

- Subject: `<type>(<optional-scope>): <imperative summary>`. Use imperative mood ("add", not "added"), lowercase after the colon, maximum 72 characters, no trailing period. Scope denotes the module/area (e.g., `core`, `media`, `render`, `export`, `cli`, `gui`, `build`).
- Types: `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `build`, `ci`, `chore`, `style`, `revert`.
- Body (after a blank line, wrapped at ~72 columns): explain what and **why**, not how. Required except for trivial one-liners.
- Breaking changes: append `!` after type/scope and include a `BREAKING CHANGE:` footer. Reference issues in the footer (`Fixes #123`, `Refs #123`).
- PR titles follow the same format; PR bodies include a **Summary** (what and why) and a **Test plan** (build/test commands executed).

### Releases

- SemVer 2.0 `MAJOR.MINOR.PATCH`: `feat` → MINOR, `fix`/`perf` → PATCH, breaking change → MAJOR (MINOR prior to 1.0.0). Pre-releases use `-rc.1`, `-beta.1`.
- Tag: annotated `vX.Y.Z` (`git tag -a v0.1.0 -m "v0.1.0"`), created on the default branch only after the release PR is merged. Never move or delete a published tag; cut a new patch instead.
- `CHANGELOG.md`: Keep a Changelog format (`Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`; newest first, `Unreleased` at the top), derived from Conventional Commits since the last tag.
- Release PR: titled `chore(release): vX.Y.Z`, solely bumps the version and moves `Unreleased` into the new release section.
- GitHub Release: `gh release create vX.Y.Z --title "vX.Y.Z" --notes-file <notes>`; pass `--prerelease` for pre-releases. Tagging and releasing are performed only when explicitly requested by the user.