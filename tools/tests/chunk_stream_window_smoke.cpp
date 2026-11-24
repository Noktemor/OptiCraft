#include "world/chunks/Chunk_Stream_Window.h"
#include <cassert>
#include <cstddef>
#include <iostream>

int main() {
    using Chunk_Stream_Window::vertical_half;
    using Chunk_Stream_Window::wanted;
    for (int r = 1; r < 24; ++r) {
        for (int h = 1; h <= 16; ++h) {
            for (int d = 0; d <= r; ++d) assert(vertical_half(r, h, d) == h);
        }
    }
    for (int r = 24; r <= 64; ++r) {
        for (int h = 1; h <= 16; ++h) {
            int last = h;
            for (int d = 0; d <= r; ++d) {
                const int now = vertical_half(r, h, d);
                assert(now >= 0 && now <= h && now <= last);
                last = now;
            }
            assert(vertical_half(r, h, 0) == h);
        }
    }
    const int r = 48, h = 8;
    size_t old_chunks = 0, new_chunks = 0;
    for (int x = -r; x <= r; ++x) for (int z = -r; z <= r; ++z) {
        for (int y = -h; y <= h; ++y) {
            ++old_chunks;
            const bool keep = wanted(r, h, std::abs(x), std::abs(y), std::abs(z));
            if (keep) ++new_chunks;
            if (std::max(std::abs(x), std::abs(z)) <= 6) assert(keep);
        }
    }
    std::cout << "window R=48 H=8: " << old_chunks << " -> " << new_chunks << " potential chunks\n";
    assert(new_chunks < old_chunks / 2);
    return 0;
}
