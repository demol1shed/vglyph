# Third-Party Licenses

vglyph itself is licensed under the GNU General Public License v3.0
(`LICENSE`). Every file vendored into this repository is listed here with its
license and a SHA-256 checksum, so a reviewer can verify it is unmodified.

## stb_truetype.h

- **File:** `third_party/stb_truetype.h`
- **Version:** v1.28 (nothings/stb, `master` as of 2026-11-23)
- **Source:** https://github.com/nothings/stb
- **License:** public domain (an MIT alternative is offered in the file footer);
  both are compatible with GPLv3.
- **SHA-256:** `428cfe93520f2aed5e413a220b0db5bba5b17e2ebe5a64e4a8efa9eeeb67ec5f`
- **Use:** parses TrueType fonts and rasterizes glyphs for visual export. Only
  `src/render/glyph_atlas.cpp` includes it.
- **Note:** the file is vendored unmodified. Two of its comments contain
  non-ASCII characters, so the MSVC build compiles `glyph_atlas.cpp` with
  `/utf-8`.

## JetBrains Mono Regular

- **File:** `assets/fonts/JetBrainsMono-Regular.ttf`
  (embedded into `vglyph-core` at build time by `cmake/embed_file.cmake`)
- **Version:** v2.304 (`fonts/ttf/JetBrainsMono-Regular.ttf` from the release zip)
- **Source:** https://github.com/JetBrains/JetBrainsMono
- **License:** SIL Open Font License 1.1 (`assets/fonts/OFL.txt`), compatible
  with GPLv3. Copyright 2020 The JetBrains Mono Project Authors.
- **SHA-256 (font):** `a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f`
- **SHA-256 (`OFL.txt`):** `30f0c136e3c88e422d0791acd97238870f9054a9729bc34cf2ff0d4ed8cac4ad`
- **Use:** the default font for `.png`, `.gif` and `.mp4` export.
