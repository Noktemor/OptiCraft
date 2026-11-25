#pragma once
#include "world/generation/Generation_Overlay_Packet.h"
#include "world/generation/Gpu_Tree_Overlay_Data.h"
#include "world/generation/Generation_Sky.h"
#include "world/generation/Generation_Light_Propagation.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

// A ticket names one completed material-buffer generation, never a world or
// a chunk ID. Only the original fallback words can differ after CPU adoption.
// Bound repair command overhead; larger repairs use the ordinary host upload.
struct Gpu_Base_Reuse {
    static constexpr size_t words=16384, max_repairs=16;
    uint64_t ticket=0;
    uint32_t level=0, repair_count=0;
    std::array<uint16_t,max_repairs> repairs{};
    static Gpu_Base_Reuse capture(uint64_t ticket, uint32_t level,
                                 std::span<const uint32_t> original) noexcept {
        Gpu_Base_Reuse r;
        if(!ticket || original.size()!=words) return r;
        r.ticket=ticket; r.level=level;
        for(size_t i=0;i<original.size();++i) {
            const uint32_t w=original[i];
            if(uint16_t(w)!=0xffffu && uint16_t(w>>16u)!=0xffffu) continue;
            if(r.repair_count==max_repairs) return {};
            r.repairs[r.repair_count++]=uint16_t(i);
        }
        return r;
    }
    bool matches(uint64_t current,size_t levels) const noexcept {
        if(!ticket || ticket!=current || level>=levels || repair_count>max_repairs) return false;
        for(size_t i=0;i<repair_count;++i)
            if(repairs[i]>=words || (i && repairs[i]<=repairs[i-1])) return false;
        return true;
    }
};

// A positive per-level certificate for retained base words, not a proof of
// CPU parity. The owning backend checks the full identity before copying.
struct Gpu_Certified_Base {
    uint64_t ticket=0;
    uint32_t level=0;
    int x=0,y=0,z=0;
    bool operator==(const Gpu_Certified_Base&) const = default;
};

// Transitional validated overlay submission. The base is authoritative and
// immutable; the GPU result must agree with exact ordered replay before adoption.
struct Gpu_Overlay_Work {
    static constexpr size_t block_words = 16384, meta_words = 8192;
    static constexpr size_t max_commands = 131072;
    static_assert(Gpu_Tree_Overlay::instance_offset==max_commands+Generation_Sky::seed_input_words);
    static constexpr size_t max_commands_per_owner = 8192;
    size_t level = 0;
    Gpu_Base_Reuse reuse;
    Generation_Overlay_Packet packet;
    std::vector<Gpu_Tree_Overlay::Instance> trees;
    std::vector<Gpu_Tree_Overlay::Selection> tree_selections;
    std::vector<uint32_t> base;
    std::vector<uint32_t> sky_input; // optional second dispatch, after overlay writes
    bool propagate = false;
    unsigned propagation_passes = 0;
    Generation_Light_Propagation::Source_Bounds light_bounds;
    size_t light_words() const noexcept {
        return Generation_Sky::output_words(sky_input.size()) + (propagate ? Generation_Light_Propagation::light_words : 0);
    }
    // Conservative upper bound; removed sources may overestimate, never underestimate.
    // Match the packed seed table (emission is capped at 14).
    unsigned max_emission(const uint8_t* emission) const noexcept {
        if(!emission) return 0;
        unsigned maximum=0;
        auto include=[&](uint16_t id){ maximum=std::max(maximum,unsigned(emission[id])); };
        for(uint32_t w:base) {
            include(uint16_t(w)); include(uint16_t(w>>16u));
            if(maximum>=14u) return 14u;
        }
        for(uint32_t w:packet.commands) {
            if(!(w&0x80000000u)) include(uint16_t(w));
            if(maximum>=14u) return 14u;
        }
        return maximum;
    }
    bool may_emit(const uint8_t* emission) const noexcept { return max_emission(emission)!=0; }
    void prepare_propagation(const uint8_t* emission, bool spatial=true) noexcept {
        light_bounds={};
        unsigned maximum=spatial?0u:max_emission(emission);
        if(spatial && emission) {
            auto include=[&](uint16_t id,unsigned index) {
                const unsigned level=std::min(unsigned(emission[id]),14u);
                maximum=std::max(maximum,level);
                if(level>1u) light_bounds.include(index);
            };
            for(unsigned i=0;i<base.size();++i) {
                include(uint16_t(base[i]),2u*i);
                include(uint16_t(base[i]>>16u),2u*i+1u);
            }
            // Include even subsequently overwritten sources: a conservative
            // bound must not omit any final emitter. Metadata is not a block.
            for(uint32_t w:packet.commands)
                if(!(w&0x80000000u)) include(uint16_t(w),(w>>16u)&32767u);
        }
        propagation_passes=maximum>1u?maximum-1u:0u;
        propagate=propagation_passes!=0;
    }
    bool prepare(size_t index, std::span<const uint32_t> words,
                 std::span<const uint32_t> ops) {
        reuse={}; trees.clear(); tree_selections.clear(); sky_input.clear(); propagate=false; propagation_passes=0; light_bounds={};
        if (words.size() != block_words || ops.empty() || ops.size() > max_commands ||
            std::any_of(words.begin(), words.end(), [](uint32_t w){
                return uint16_t(w)==0xffffu || uint16_t(w>>16u)==0xffffu;
            })) return false;
        if (std::any_of(ops.begin(),ops.end(),[](uint32_t w){return Gpu_Tree_Overlay::marker(w)||Gpu_Tree_Overlay::air_write(w);}) || !packet.compile(ops)) return false;
        for (const auto& task : packet.tasks)
            if (task.end-task.begin > max_commands_per_owner) return false;
        level = index; base.assign(words.begin(), words.end()); return true;
    }
    // Snapshot-only admission has no full CPU replay after execution. Validate
    // the CSR ranges and owner partition before the shader can index them.
    // This is a structural certificate, not a proof of procedural correctness.
    bool valid_snapshot_input() const noexcept {
        if (base.size()!=block_words) return false;
        for (uint32_t word:base)
            if (uint16_t(word)==0xffffu || uint16_t(word>>16u)==0xffffu) return false;
        return trees.empty() && valid_snapshot_commands() &&
            std::none_of(packet.commands.begin(),packet.commands.end(),Gpu_Tree_Overlay::air_write);
    }
    bool valid_certified_snapshot_input() const noexcept {
        return base.empty() && valid_snapshot_commands();
    }
    bool valid_snapshot_commands() const noexcept {
        if (packet.tasks.size()>Generation_Overlay_Packet::group_count ||
            packet.tasks.empty()!=packet.commands.empty() || packet.commands.size()>max_commands ||
            !sky_input.empty() || propagate || reuse.ticket || !Gpu_Tree_Overlay::valid(trees) || !Gpu_Tree_Overlay::valid_selections(tree_selections,trees)) return false;
        std::array<bool,Gpu_Tree_Overlay::max_instances> referenced{}, variantOnly{};
        for(auto s:tree_selections)variantOnly[s.instance]=Gpu_Tree_Overlay::independent_selection(s.biome);
        size_t cursor=0; uint32_t previous=0;
        for (const auto& task:packet.tasks) {
            if (task.group>=Generation_Overlay_Packet::group_count ||
                (cursor && task.group<=previous) || task.begin!=cursor ||
                task.end<=task.begin || task.end>packet.commands.size() ||
                task.end-task.begin>max_commands_per_owner) return false;
            for (size_t j=task.begin;j<task.end;++j) {
                const uint32_t op=packet.commands[j];
                if (((op>>18u)&8191u)!=task.group ||
                    (!(op&0x80000000u) && uint16_t(op)==0xffffu) ||
                    (Gpu_Tree_Overlay::marker(op) && !Gpu_Tree_Overlay::valid_marker(op,trees.size()))) return false;
                if(Gpu_Tree_Overlay::marker(op)){
                    const auto i=Gpu_Tree_Overlay::index(op);
                    if((op&Gpu_Tree_Overlay::air_flag) && trees[i].handle>=Gpu_Tree_Overlay::templates)return false;
                    if(variantOnly[i] && !(op&Gpu_Tree_Overlay::air_flag))return false;
                    referenced[i]=true;
                }
            }
            previous=task.group; cursor=task.end;
        }
        for(auto s:tree_selections)if(!referenced[s.instance])return false;
        return cursor==packet.commands.size();
    }
    bool prepare_snapshot(size_t index,std::span<const uint32_t> words,
                          std::span<const uint32_t> ops) {
        if (!ops.empty()) return prepare(index,words,ops) && valid_snapshot_input();
        if (words.size()!=block_words) return false;
        reuse={}; trees.clear(); tree_selections.clear(); sky_input.clear(); propagate=false; propagation_passes=0; light_bounds={};
        packet.compile(ops);level=index;base.assign(words.begin(),words.end());
        return valid_snapshot_input();
    }
    // Build ordered commands for a retained GPU base, without allocating or
    // validating a nonexistent CPU base. Preserve prior work on refusal.
    bool prepare_certified_snapshot(size_t index,std::span<const uint32_t> ops,
                                    std::vector<Gpu_Tree_Overlay::Instance> instances={},
                                    std::vector<Gpu_Tree_Overlay::Selection> selections={}) {
        if (ops.size()>max_commands) return false;
        Gpu_Overlay_Work next;
        next.level=index;next.trees=std::move(instances);next.tree_selections=std::move(selections);
        if (!next.packet.compile(ops) || !next.valid_certified_snapshot_input()) return false;
        next.packet.compact_zero_meta_snapshot();
        *this=std::move(next);return true;
    }
    // No tree shaders/catalog allocation required on the fallback device.
    // Keep the certified base on the GPU; only reconstruct placement commands.
    bool expand_tree_snapshot(std::span<const uint32_t> catalog,Gpu_Overlay_Work& output) const {
        if(!valid_certified_snapshot_input())return false;
        auto resolved=trees;
        for(auto s:tree_selections)if(Gpu_Tree_Overlay::independent_selection(s.biome)){
            auto& a=resolved[s.instance];
            const auto species=Gpu_Tree_Overlay::candidate_selection(s.biome)?Gpu_Tree_Overlay::fallback_species(s.x,s.z,s.seed,s.biome&255u):a.handle/36;
            a.handle=species==Gpu_Tree_Overlay::no_tree?species:species*36+Gpu_Tree_Overlay::fallback_variant(s.x,s.z,species,s.seed);
        }
        std::vector<uint32_t> ops;ops.reserve(packet.commands.size());
        for(uint32_t op:packet.commands){
            if(Gpu_Tree_Overlay::marker(op)){
                if(resolved[Gpu_Tree_Overlay::index(op)].handle==Gpu_Tree_Overlay::no_tree)continue;
                if(!Gpu_Tree_Overlay::expand_marker(op,resolved,catalog,ops,max_commands))return false;
            }else{
                if(ops.size()>=max_commands)return false;
                ops.push_back(op);
            }
        }
        return output.prepare_certified_snapshot(level,ops);
    }
    bool verify(std::span<const uint32_t> output, uint64_t& edits) const {
        if (!trees.empty() || base.size()!=block_words || !Generation_Sky::valid_input(sky_input.size()) ||
            (propagate && (!Generation_Sky::has_seeds(sky_input.size()) ||
                           propagation_passes==0 || propagation_passes>Generation_Light_Propagation::max_block_passes)) ||
            output.size()!=block_words+meta_words+packet.tasks.size()+light_words()) return false;
        size_t t=0; uint64_t total=0;
        for (size_t g=0;g<meta_words;++g) {
            uint32_t a=base[2*g], b=base[2*g+1], m=0;
            if(t<packet.tasks.size() && packet.tasks[t].group==g) {
                const auto& task=packet.tasks[t];
                const uint32_t n=Generation_Overlay_Packet::apply(
                    std::span<const uint32_t>(packet.commands).subspan(task.begin,task.end-task.begin),a,b,m);
                if(output[block_words+meta_words+t]!=n) return false;
                total+=n; ++t;
            }
            if(output[2*g]!=a || output[2*g+1]!=b || output[block_words+g]!=m) return false;
        }
        if(t!=packet.tasks.size()) return false;
        edits=total; return true;
    }
};
