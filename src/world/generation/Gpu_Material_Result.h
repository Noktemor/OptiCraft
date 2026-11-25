#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// CPU view of the GPU's 2x16-bit words (same x-major order as the voxel arena).
// ID semantics are chosen by the caller: legacy material 0 = CPU fallback;
// Vulkan base result 0 = air, 0xffff = CPU fallback. This view does not interpret IDs.
// Only dispatched Y levels own data. No allocation for the other column levels.
class Gpu_Material_Result {
public:
    static constexpr size_t voxels = 32768;
    static constexpr size_t words_per_chunk = voxels / 2;
    static constexpr size_t max_levels = 9;
    enum class Encoding { Packed16, Legacy32 };

    // Transactional: invalid size, duplicate indices or out-of-range IDs leave
    // the previous view unchanged. The input is consumed only on success.
    bool assign(size_t levels, const std::vector<size_t>& indices,
                std::vector<uint32_t>&& words, Encoding encoding) {
        if (!levels || levels > max_levels || indices.empty() || indices.size() > levels)
            return false;
        const size_t count = indices.size() * words_per_chunk;
        if (words.size() != count * (encoding == Encoding::Legacy32 ? 2 : 1)) return false;
        std::array<size_t, max_levels> offsets;
        offsets.fill(missing);
        for (size_t i = 0; i < indices.size(); ++i) {
            const size_t index = indices[i];
            if (index >= levels || offsets[index] != missing) return false;
            offsets[index] = i * words_per_chunk;
        }
        if (encoding == Encoding::Legacy32) {
            for (uint32_t id : words) if (id > 0xffffu) return false;
            // GL's existing output is converted in-place (no second full array).
            for (size_t i = 0; i < count; ++i)
                words[i] = words[2*i] | (words[2*i+1] << 16u);
            words.resize(count);
        }
        words_ = std::move(words);
        offsets_ = offsets;
        return true;
    }
    // The build worker obtains this once per chunk, outside the voxel loop.
    // The pointer remains valid while the immutable Prepared_Column lives.
    [[nodiscard]] const uint32_t* level_words(size_t level) const noexcept {
        return level < max_levels && offsets_[level] != missing
            ? words_.data() + offsets_[level] : nullptr;
    }
    // Only for the validation worker's private, unpublished result.
    [[nodiscard]] uint32_t* mutable_level_words(size_t level) noexcept {
        return level < max_levels && offsets_[level] != missing
            ? words_.data() + offsets_[level] : nullptr;
    }
    [[nodiscard]] static uint16_t unpack(const uint32_t* words, size_t voxel) noexcept {
        return uint16_t(words[voxel/2] >> ((voxel & 1u)*16u));
    }
    struct Decoded_Layer { int non_air = 0; bool has_fallback = false; };
    // Base encoding only. Decode a complete even-sized layer without coordinate,
    // cave, biome or sky checks. Keep fallback markers for the exact CPU repair.
    template<size_t Layer> [[nodiscard]] static Decoded_Layer decode_base_layer(
            const uint32_t* words, uint16_t* out) noexcept {
        static_assert(Layer % 2 == 0);
        Decoded_Layer result;
        for (size_t i = 0; i < Layer / 2; ++i) {
            const uint32_t word = words[i];
            const uint16_t lo = uint16_t(word), hi = uint16_t(word >> 16u);
            out[2*i] = lo; out[2*i+1] = hi;
            result.non_air += (lo != 0 && lo != 0xffffu) + (hi != 0 && hi != 0xffffu);
            result.has_fallback |= lo == 0xffffu || hi == 0xffffu;
        }
        return result;
    }
    [[nodiscard]] uint16_t at(size_t level, size_t voxel) const noexcept {
        const auto* words = level_words(level);
        return words && voxel < voxels ? unpack(words, voxel) : 0;
    }
    [[nodiscard]] bool has_level(size_t level) const noexcept {
        return level_words(level) != nullptr;
    }
    // Check all IDs before the sampled numerical parity check: a bad GPU result
    // must not inject an arbitrary block ID into generated/saved chunks.
    template<class Predicate> [[nodiscard]] bool all_ids(Predicate allowed) const {
        for (uint32_t word : words_)
            if (!allowed(uint16_t(word)) || !allowed(uint16_t(word >> 16u))) return false;
        return true;
    }
    [[nodiscard]] size_t word_count() const noexcept { return words_.size(); }
private:
    static constexpr size_t missing = static_cast<size_t>(-1);
    std::array<size_t, max_levels> offsets_ = [] {
        std::array<size_t, max_levels> result{};
        result.fill(missing);
        return result;
    }();
    std::vector<uint32_t> words_;
};
