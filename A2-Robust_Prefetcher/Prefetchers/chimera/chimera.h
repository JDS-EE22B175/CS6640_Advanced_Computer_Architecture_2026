#ifndef CHIMERA_H
#define CHIMERA_H

#include <cstdint>
#include <vector>
#include <array>
#include "champsim.h"
#include "modules.h"

struct chimera : public champsim::modules::prefetcher {
    
    // ── 1. DCPT Engine (Local Complex Patterns) ──
    static constexpr int PC_ENTRIES = 256;
    struct pc_entry {
        uint64_t pc = 0;
        uint64_t last_block = 0;
        std::array<int64_t, 4> deltas = {0};
        int delta_head = 0;
        int conf = 0;
        int lru = 0;
        bool valid = false;
    };
    std::vector<pc_entry> dcpt_table;

    // ── 2. True BOP Engine (Global Phase Learning) ──
    std::array<int, 20> bop_offsets = {1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 8, -8, 10, -10, 12, -12, 16, -16};
    int bop_current_idx = 0;
    int bop_round_score = 0;
    int bop_phase_timer = 0;
    int bop_best_score = 0;
    int bop_active_offset = 1; 
    
    std::array<uint64_t, 256> rr_buffer = {0};
    int rr_head = 0;

    using champsim::modules::prefetcher::prefetcher;

    explicit chimera(CACHE* cache) : champsim::modules::prefetcher(cache), dcpt_table(PC_ENTRIES) {}

    uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type, uint32_t metadata_in);
    uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in) { return metadata_in; }
    void prefetcher_cycle_operate() {}
};

#endif