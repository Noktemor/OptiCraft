#include "world/generation/Structure_Pack.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
constexpr size_t kHeaderSize = 16;
constexpr size_t kIndexSize = 44;
constexpr size_t kRecordSize = 10;
constexpr size_t kMaxSize = 32 * 1024 * 1024;
constexpr uint32_t kMaxBlocks = 250000;
constexpr const char* kNames[] = {"Cottage_Oak", "Cottage_Birch", "Farm", "Well", "Fortress"};
static_assert(std::size(kNames) == static_cast<size_t>(Structure_Template_Id::Count));

uint16_t u16(const std::vector<uint8_t>& v, size_t at) {
    return static_cast<uint16_t>(v[at]) | static_cast<uint16_t>(v[at + 1] << 8);
}
int16_t i16(const std::vector<uint8_t>& v, size_t at) {
    return static_cast<int16_t>(u16(v, at));
}
uint32_t u32(const std::vector<uint8_t>& v, size_t at) {
    return static_cast<uint32_t>(u16(v, at)) | (static_cast<uint32_t>(u16(v, at + 2)) << 16);
}
}

const Structure_Pack& Structure_Pack::instance() {
    static const Structure_Pack pack;
    return pack;
}

const Structure_Template* Structure_Pack::get(Structure_Template_Id id) const {
    const auto index = static_cast<size_t>(id);
    return m_ready && index < m_templates.size() ? &m_templates[index] : nullptr;
}

Structure_Pack::Structure_Pack() {
    std::ifstream file("assets/structures/structures.struct", std::ios::binary | std::ios::ate);
    if (!file) return;
    const auto length = file.tellg();
    if (length < static_cast<std::streamoff>(kHeaderSize) ||
        length > static_cast<std::streamoff>(kMaxSize)) return;
    std::vector<uint8_t> v(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size()))) return;
    if (std::memcmp(v.data(), "OSTR", 4) != 0 || u16(v, 4) != 1) return;
    const uint16_t count = u16(v, 6);
    const size_t index_at = u32(v, 8), data_at = u32(v, 12);
    if (count != m_templates.size() || index_at < kHeaderSize ||
        index_at > v.size() || size_t(count) * kIndexSize > v.size() - index_at ||
        data_at < index_at + size_t(count) * kIndexSize || data_at > v.size()) return;

    std::array<Structure_Template, static_cast<size_t>(Structure_Template_Id::Count)> parsed;
    std::array<bool, static_cast<size_t>(Structure_Template_Id::Count)> seen{};
    for (size_t i = 0; i < count; ++i) {
        const size_t base = index_at + i * kIndexSize;
        const char* name_ptr = reinterpret_cast<const char*>(v.data() + base);
        const void* terminator = std::memchr(name_ptr, 0, 24);
        if (!terminator) return;
        const std::string name(name_ptr);
        size_t id = count;
        for (size_t j = 0; j < count; ++j) {
            if (name == kNames[j]) { id = j; break; }
        }
        if (id == count || seen[id]) return;
        seen[id] = true;
        auto& t = parsed[id];
        t.name = name;
        t.width = u16(v, base + 24);
        t.height = u16(v, base + 26);
        t.depth = u16(v, base + 28);
        t.entrance_x = i16(v, base + 30);
        t.entrance_z = i16(v, base + 32);
        const size_t offset = u32(v, base + 34);
        const uint32_t blocks = u32(v, base + 38);
        const uint16_t flags = u16(v, base + 42);
        if (flags || !t.width || !t.height || !t.depth ||
            t.width > 512 || t.height > 256 || t.depth > 512 ||
            t.entrance_x < 0 || t.entrance_x >= t.width ||
            t.entrance_z < 0 || t.entrance_z >= t.depth ||
            !blocks || blocks > kMaxBlocks || offset < data_at ||
            offset > v.size() || size_t(blocks) * kRecordSize > v.size() - offset) return;
        t.voxels.reserve(blocks);
        for (uint32_t j = 0; j < blocks; ++j) {
            const size_t at = offset + j * kRecordSize;
            Structure_Voxel b{i16(v, at), i16(v, at+2), i16(v, at+4),
                static_cast<Block_Types>(u16(v, at+6)), v[at+8], v[at+9]};
            if (b.flags > 1 || b.x < 0 || b.x >= t.width ||
                b.y < -8 || b.y >= t.height || b.z < 0 || b.z >= t.depth) return;
            t.voxels.push_back(b);
        }
    }
    for (bool s : seen) if (!s) return;
    m_templates = std::move(parsed);
    m_ready = true;
}

void Structure_Pack::rotate_xz(int x, int z, uint8_t rotation, int& out_x, int& out_z) {
    switch (rotation & 3u) {
        case 0: out_x = x; out_z = z; break;
        case 1: out_x = -z; out_z = x; break;
        case 2: out_x = -x; out_z = -z; break;
        default: out_x = z; out_z = -x; break;
    }
}

void Structure_Pack::world_position(const Structure_Template& t, const Structure_Instance& inst,
                                    const Structure_Voxel& b, int& x, int& y, int& z) {
    int rx, rz;
    rotate_xz(static_cast<int>(b.x) - t.entrance_x,
              static_cast<int>(b.z) - t.entrance_z, inst.rotation, rx, rz);
    x = inst.entrance_world_x + rx;
    y = inst.base_world_y + b.y;
    z = inst.entrance_world_z + rz;
}
