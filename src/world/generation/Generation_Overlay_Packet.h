#pragma once
#include "world/generation/Gpu_Tree_Overlay_Data.h"
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

// Sparse CSR tasks: one owner for four consecutive voxels (two block words,
// one metadata word). Stable scatter preserves order within each owner.
// Input must be a fully resolved base, not a worldgen fallback-marked array.
struct Generation_Overlay_Packet {
    static constexpr uint32_t group_count = 8192;
    struct Task { uint32_t group, begin, end; };
    static_assert(sizeof(Task) == 12);
    std::vector<Task> tasks;
    std::vector<uint32_t> commands;

    bool compile(std::span<const uint32_t> input) {
        tasks.clear(); commands.clear();
        if (input.size() > std::numeric_limits<uint32_t>::max()) return false;
        if (input.empty()) return true;
        std::array<uint32_t, group_count> cursor{};
        for (uint32_t w : input) ++cursor[(w >> 18u) & 8191u];
        uint32_t offset = 0;
        for (uint32_t group = 0; group < group_count; ++group) {
            const uint32_t count = cursor[group];
            cursor[group] = offset;
            if (count) tasks.push_back({group, offset, offset + count});
            offset += count;
        }
        commands.resize(input.size());
        for (uint32_t w : input) commands[cursor[(w >> 18u) & 8191u]++] = w;
        return true;
    }

    // Snapshot metadata starts at zero. Reduce each independent voxel to
    // its final ID and metadata expression, retaining the one conditional
    // reset (meta followed by the first block write) when base ID is unknown.
    // Not for validated readback: intermediate change counters are different.
    void compact_zero_meta_snapshot() {
        struct Lane {
            uint32_t last_block=0, last_meta=0;
            bool has_block=false, has_meta=false, conditional=false;
            void consume(uint32_t op) noexcept {
                if(op&0x80000000u){
                    last_meta=op;has_meta=(op&255u)!=0;conditional=false;
                }else{
                    if(has_block && uint16_t(last_block)!=uint16_t(op)){
                        has_meta=false;conditional=false;
                    }else if(!has_block && has_meta)conditional=true;
                    last_block=op;has_block=true;
                }
            }
            void emit(std::vector<uint32_t>& out)const {
                if(has_meta && conditional)out.push_back(last_meta);
                if(has_block)out.push_back(last_block);
                if(has_meta && !conditional)out.push_back(last_meta);
            }
        };
        std::vector<uint32_t> next;next.reserve(commands.size());
        size_t n=0;
        for(const auto task:tasks){
            bool hasTemplate=false;
            for(size_t j=task.begin;j<task.end;++j)if(Gpu_Tree_Overlay::marker(commands[j])||Gpu_Tree_Overlay::air_write(commands[j])){hasTemplate=true;break;}
            if(hasTemplate){
                auto begin=uint32_t(next.size());
                next.insert(next.end(),commands.begin()+task.begin,commands.begin()+task.end);
                tasks[n++]={task.group,begin,uint32_t(next.size())};continue;
            }
            std::array<Lane,4> lanes{};
            for(size_t j=task.begin;j<task.end;++j)lanes[(commands[j]>>16u)&3u].consume(commands[j]);
            const auto begin=uint32_t(next.size());
            for(const auto& lane:lanes)lane.emit(next);
            if(next.size()!=begin)tasks[n++]={task.group,begin,uint32_t(next.size())};
        }
        tasks.resize(n);commands.swap(next);
    }

    // Exact ordered replay; return number of logical ID/meta changes, including
    // intermediate changes that cancel out before publication.
    static uint32_t apply(std::span<const uint32_t> ops, uint32_t& b0,
                          uint32_t& b1, uint32_t& meta) noexcept {
        uint32_t changed = 0;
        const uint32_t base[2]{b0,b1};
        for (uint32_t op : ops) {
            const uint32_t lane = (op >> 16u) & 3u;
            const uint32_t ms = lane * 8u;
            const uint32_t mm = 255u << ms;
            const bool conditional=Gpu_Tree_Overlay::air_write(op);
            if(conditional && ((base[lane/2]>>((lane&1u)*16u))&65535u)!=0u)continue;
            if ((op & 0x80000000u) && !conditional) {
                const uint32_t next = (meta & ~mm) | ((op & 255u) << ms);
                changed += next != meta; meta = next;
            } else {
                uint32_t& block = lane < 2u ? b0 : b1;
                const uint32_t bs = (lane & 1u) * 16u;
                const uint32_t bm = 65535u << bs;
                const uint32_t next = (block & ~bm) | ((conditional?Gpu_Tree_Overlay::air_id(op):(op & 65535u)) << bs);
                if (next != block) { block = next; meta &= ~mm; ++changed; }
            }
        }
        return changed;
    }
};
