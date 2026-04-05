#include "chimera.h"
#include "cache.h"
#include <algorithm>

uint32_t chimera::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type, uint32_t metadata_in) {
    if (type != access_type::LOAD) return metadata_in;

    using namespace champsim::data::data_literals;
    uint64_t block = champsim::block_number{addr}.to<uint64_t>();
    uint64_t pc = ip.slice_upper<2_b>().to<uint64_t>();
    double mshr_load = intern_->get_mshr_occupancy_ratio();

    if (mshr_load > 0.85) return metadata_in;

    bool dcpt_fired = false;

    // ═══════════════════════════════════════════════════════════════════════
    // ENGINE 1: DCPT (Local Pattern Matching)
    // ═══════════════════════════════════════════════════════════════════════
    int idx = -1, lru_idx = 0;
    for (int i = 0; i < PC_ENTRIES; i++) {
        if (dcpt_table[i].valid && dcpt_table[i].pc == pc) idx = i;
        if (dcpt_table[i].lru > dcpt_table[lru_idx].lru) lru_idx = i;
        if (dcpt_table[i].valid) dcpt_table[i].lru++;
    }

    if (idx == -1) {
        dcpt_table[lru_idx] = {pc, block, {0}, 0, 0, 0, true};
    } else {
        auto& e = dcpt_table[idx];
        e.lru = 0;
        int64_t delta = static_cast<int64_t>(block) - static_cast<int64_t>(e.last_block);
        e.last_block = block;

        if (delta != 0) {
            e.deltas[e.delta_head] = delta;
            e.delta_head = (e.delta_head + 1) % 4;
            
            // Check for basic 2-delta repeating pattern (A, B, A, B)
            int prev1 = (e.delta_head - 1 + 4) % 4;
            int prev2 = (e.delta_head - 2 + 4) % 4;
            int prev3 = (e.delta_head - 3 + 4) % 4;

            if (e.deltas[prev1] == e.deltas[prev3] && e.deltas[prev1] != 0) {
                e.conf = std::min(e.conf + 2, 10);
                if (e.conf >= 4) {
                    uint64_t target1 = block + e.deltas[prev2];
                    uint64_t target2 = target1 + e.deltas[prev1];
                    
                    champsim::address pf1{champsim::block_number{target1}};
                    if (champsim::page_number{pf1} == champsim::page_number{addr}) prefetch_line(pf1, true, metadata_in);
                    
                    champsim::address pf2{champsim::block_number{target2}};
                    if (champsim::page_number{pf2} == champsim::page_number{addr}) prefetch_line(pf2, true, metadata_in);
                    
                    dcpt_fired = true;
                }
            } else {
                e.conf = std::max(e.conf - 1, 0);
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // ENGINE 2: True BOP (Global Phase Learning)
    // ═══════════════════════════════════════════════════════════════════════
    uint64_t test_past = block - bop_offsets[bop_current_idx];
    bool hit_bop = false;
    for (int i = 0; i < 256; i++) {
        if (rr_buffer[i] == test_past) { hit_bop = true; break; }
    }
    if (hit_bop) bop_round_score++;

    rr_buffer[rr_head] = block;
    rr_head = (rr_head + 1) % 256;

    bop_phase_timer++;
    if (bop_phase_timer >= 256) {
        if (bop_round_score > bop_best_score) {
            bop_best_score = bop_round_score;
            bop_active_offset = bop_offsets[bop_current_idx];
        }
        bop_round_score = 0;
        bop_phase_timer = 0;
        bop_current_idx++;

        if (bop_current_idx >= bop_offsets.size()) {
            if (bop_best_score < 15) bop_active_offset = 1; // Dirty Fallback
            bop_best_score = 0;
            bop_current_idx = 0;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // ARBITRATION
    // ═══════════════════════════════════════════════════════════════════════
    // If DCPT (local) hasn't figured out the complex pattern yet, hand the 
    // reigns over to the BOP Engine (global) to blast streams or scrape +1s.
    if (!dcpt_fired) {
        int degree = (bop_active_offset == 1) ? 1 : 3;
        for (int i = 1; i <= degree; i++) {
            uint64_t target = block + (bop_active_offset * i);
            champsim::address pf_addr{champsim::block_number{target}};
            if (champsim::page_number{pf_addr} == champsim::page_number{addr}) {
                prefetch_line(pf_addr, true, metadata_in);
            }
        }
    }

    return metadata_in;
}