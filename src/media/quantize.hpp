#pragma once

// Internal helper (not installed): reduces an RGBA image to a 256-color
// palette for GIF export. No dithering; deterministic for equal input.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vglyph {

inline constexpr std::size_t kPaletteSize = 256;

class Quantizer {
public:
    // Every buffer is allocated here, so quantize() never allocates.
    Quantizer();

    // Writes one palette index per pixel of `rgba` (alpha is ignored) into
    // `indices`, and the palette into `palette` as kPaletteSize native-endian
    // 0xAARRGGBB entries (the AV_PIX_FMT_PAL8 layout) with alpha 255.
    //
    // An image with at most 256 distinct colors keeps them exactly. Otherwise
    // the colors are grouped by median cut over a 5-bit-per-channel histogram
    // and each palette entry is the mean of the pixels it represents.
    // Unused palette entries are opaque black.
    void quantize(const std::uint8_t* rgba, std::size_t pixels, std::uint8_t* indices,
                  std::uint32_t* palette);

private:
    struct Box {
        std::size_t begin = 0;  // range in occupied_
        std::size_t end = 0;
        std::uint64_t pixels = 0;
    };

    // Returns false if the image has more than kPaletteSize distinct colors.
    [[nodiscard]] bool quantize_exact(const std::uint8_t* rgba, std::size_t pixels,
                                      std::uint8_t* indices, std::uint32_t* palette);
    void quantize_median_cut(const std::uint8_t* rgba, std::size_t pixels, std::uint8_t* indices,
                             std::uint32_t* palette);
    void split_box(std::size_t box_index);

    // Exact path: open-addressing table from color to palette index.
    std::array<std::uint32_t, 1024> hash_keys_{};
    std::array<std::uint8_t, 1024> hash_values_{};

    // Median-cut path: one histogram bucket per 5-bit RGB value.
    std::vector<std::uint32_t> count_;
    std::vector<std::uint64_t> sum_r_;
    std::vector<std::uint64_t> sum_g_;
    std::vector<std::uint64_t> sum_b_;
    std::vector<std::uint16_t> occupied_;  // buckets with a nonzero count
    std::vector<std::uint8_t> bucket_index_;
    std::vector<Box> boxes_;
};

}  // namespace vglyph
