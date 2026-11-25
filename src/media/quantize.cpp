#include "quantize.hpp"

#include <algorithm>

namespace vglyph {

namespace {

constexpr std::size_t kBuckets = 32768;  // 32 levels per channel
constexpr std::uint32_t kOpaqueBlack = 0xFF000000u;

[[nodiscard]] std::uint32_t pack(const std::uint8_t* px) noexcept {
    return kOpaqueBlack | (static_cast<std::uint32_t>(px[0]) << 16) |
           (static_cast<std::uint32_t>(px[1]) << 8) | static_cast<std::uint32_t>(px[2]);
}

[[nodiscard]] std::size_t bucket_of(const std::uint8_t* px) noexcept {
    return (static_cast<std::size_t>(px[0] >> 3) << 10) | (static_cast<std::size_t>(px[1] >> 3) << 5) |
           static_cast<std::size_t>(px[2] >> 3);
}

// Channel `c` (0 red, 1 green, 2 blue) of a bucket number, 0..31.
[[nodiscard]] unsigned channel_of(std::size_t bucket, int c) noexcept {
    return static_cast<unsigned>((bucket >> (10 - 5 * c)) & 31u);
}

[[nodiscard]] std::size_t hash_slot(std::uint32_t key) noexcept {
    return static_cast<std::size_t>((key * 2654435761u) >> 22);  // top 10 bits
}

}  // namespace

Quantizer::Quantizer()
    : count_(kBuckets, 0),
      sum_r_(kBuckets, 0),
      sum_g_(kBuckets, 0),
      sum_b_(kBuckets, 0),
      bucket_index_(kBuckets, 0) {
    occupied_.reserve(kBuckets);
    boxes_.reserve(kPaletteSize);
}

bool Quantizer::quantize_exact(const std::uint8_t* rgba, std::size_t pixels, std::uint8_t* indices,
                               std::uint32_t* palette) {
    hash_keys_.fill(0);  // a stored key always has alpha 255, so 0 means empty
    std::size_t distinct = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint32_t key = pack(rgba + i * 4);
        std::size_t slot = hash_slot(key);
        while (hash_keys_[slot] != 0 && hash_keys_[slot] != key) {
            slot = (slot + 1) & (hash_keys_.size() - 1);
        }
        if (hash_keys_[slot] == 0) {
            if (distinct == kPaletteSize) {
                return false;
            }
            hash_keys_[slot] = key;
            hash_values_[slot] = static_cast<std::uint8_t>(distinct);
            palette[distinct] = key;
            ++distinct;
        }
        indices[i] = hash_values_[slot];
    }
    std::fill(palette + distinct, palette + kPaletteSize, kOpaqueBlack);
    return true;
}

void Quantizer::split_box(std::size_t box_index) {
    const Box box = boxes_[box_index];

    unsigned lo[3] = {31, 31, 31};
    unsigned hi[3] = {0, 0, 0};
    for (std::size_t i = box.begin; i < box.end; ++i) {
        for (int c = 0; c < 3; ++c) {
            const unsigned v = channel_of(occupied_[i], c);
            lo[c] = std::min(lo[c], v);
            hi[c] = std::max(hi[c], v);
        }
    }
    int widest = 0;
    for (int c = 1; c < 3; ++c) {
        if (hi[c] - lo[c] > hi[widest] - lo[widest]) {
            widest = c;
        }
    }

    // The bucket number breaks ties, so the order does not depend on the sort.
    std::sort(occupied_.begin() + static_cast<std::ptrdiff_t>(box.begin),
              occupied_.begin() + static_cast<std::ptrdiff_t>(box.end),
              [widest](std::uint16_t a, std::uint16_t b) {
                  const unsigned ca = channel_of(a, widest);
                  const unsigned cb = channel_of(b, widest);
                  return ca != cb ? ca < cb : a < b;
              });

    // Split where half of the box's pixels lie on each side; both halves keep at least one bucket.
    std::uint64_t below = 0;
    std::size_t cut = box.begin;
    while (cut + 1 < box.end && below * 2 < box.pixels) {
        below += count_[occupied_[cut]];
        ++cut;
    }
    boxes_[box_index] = Box{box.begin, cut, below};
    boxes_.push_back(Box{cut, box.end, box.pixels - below});
}

void Quantizer::quantize_median_cut(const std::uint8_t* rgba, std::size_t pixels,
                                    std::uint8_t* indices, std::uint32_t* palette) {
    for (const std::uint16_t b : occupied_) {  // clear what the previous call used
        count_[b] = 0;
        sum_r_[b] = 0;
        sum_g_[b] = 0;
        sum_b_[b] = 0;
    }
    occupied_.clear();
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint8_t* px = rgba + i * 4;
        const std::size_t b = bucket_of(px);
        if (count_[b] == 0) {
            occupied_.push_back(static_cast<std::uint16_t>(b));  // capacity is kBuckets
        }
        ++count_[b];
        sum_r_[b] += px[0];
        sum_g_[b] += px[1];
        sum_b_[b] += px[2];
    }

    boxes_.clear();
    boxes_.push_back(Box{0, occupied_.size(), pixels});
    while (boxes_.size() < kPaletteSize) {
        std::size_t best = boxes_.size();  // the splittable box with the most pixels
        for (std::size_t i = 0; i < boxes_.size(); ++i) {
            const bool splittable = boxes_[i].end - boxes_[i].begin > 1;
            if (splittable && (best == boxes_.size() || boxes_[i].pixels > boxes_[best].pixels)) {
                best = i;
            }
        }
        if (best == boxes_.size()) {
            break;
        }
        split_box(best);
    }

    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        std::uint64_t r = 0;
        std::uint64_t g = 0;
        std::uint64_t b = 0;
        std::uint64_t n = 0;
        for (std::size_t j = boxes_[i].begin; j < boxes_[i].end; ++j) {
            const std::uint16_t bucket = occupied_[j];
            r += sum_r_[bucket];
            g += sum_g_[bucket];
            b += sum_b_[bucket];
            n += count_[bucket];
            bucket_index_[bucket] = static_cast<std::uint8_t>(i);
        }
        const std::uint64_t half = n / 2;
        palette[i] = kOpaqueBlack | (static_cast<std::uint32_t>((r + half) / n) << 16) |
                     (static_cast<std::uint32_t>((g + half) / n) << 8) |
                     static_cast<std::uint32_t>((b + half) / n);
    }
    std::fill(palette + boxes_.size(), palette + kPaletteSize, kOpaqueBlack);

    for (std::size_t i = 0; i < pixels; ++i) {
        indices[i] = bucket_index_[bucket_of(rgba + i * 4)];
    }
}

void Quantizer::quantize(const std::uint8_t* rgba, std::size_t pixels, std::uint8_t* indices,
                         std::uint32_t* palette) {
    if (pixels == 0) {
        std::fill(palette, palette + kPaletteSize, kOpaqueBlack);
        return;
    }
    if (!quantize_exact(rgba, pixels, indices, palette)) {
        quantize_median_cut(rgba, pixels, indices, palette);
    }
}

}  // namespace vglyph
