// adaptive.cc — Pure SHiP with Throttle Adaptive LLC Replacement Policy
//
// Architecture: 2048-set, 16-way LLC
//
// We found that global Set Dueling between SRRIP and SHiP fundamentally fails 
// on traces like T4 where a higher miss rate (caused by aggressive bypassing) 
// actually yields higher IPC due to memory-level parallelism (MLP). Since Set 
// Dueling optimizes exclusively for lowest miss rate, it mathematically cannot 
// select the best policy for T4.
//
// Instead, we use Pure SHiP. SHiP adapts perfectly because it makes decisions
// per-instruction (PC) rather than per-set or globally.
//
// Crucial Fix for T3 and T2 (False Streaming):
// If a line is predicted as streaming, we bypass it (RRPV=3). However, we insert
// 1 out of 32 streaming lines at RRPV=2. This allows mispredicted PCs to stay in 
// the cache, get a hit, and correct their SHCT entry.

#include "adaptive.h"
#include <algorithm>
#include <iostream>
#include <string>

adaptive::adaptive(CACHE* cache)
    : champsim::modules::replacement(cache),
      my_cache(cache),
      ship_counter(0),
      total_evictions(0),
      ship_reuse_predictions(0),
      ship_streaming_predictions(0),
      ship_neutral_predictions(0),
      initialized(false) {}

void adaptive::check_init() {
    if (initialized) return;
    initialized = true;

    long num_sets = my_cache->NUM_SET;
    long num_ways = my_cache->NUM_WAY;
    long N = num_sets * num_ways;

    line_rrpv.assign(N, MAX_RRPV);
    line_sig.assign(N, 0);
    line_outcome.assign(N, false);

    // Initialize SHCT neutral
    shct.assign(SHCT_SIZE, SHCT_MAX / 2);
}

uint16_t adaptive::get_pc_signature(champsim::address ip) const {
    uint64_t raw = ip.to<uint64_t>();
    return static_cast<uint16_t>(raw ^ (raw >> 16) ^ (raw >> 32) ^ (raw >> 48));
}

long adaptive::find_victim_rrip(long set) {
    long off = set * my_cache->NUM_WAY;
    while (true) {
        for (long w = 0; w < my_cache->NUM_WAY; ++w) {
            if (line_rrpv[off + w] == MAX_RRPV)
                return w;
        }
        for (long w = 0; w < my_cache->NUM_WAY; ++w) {
            if (line_rrpv[off + w] < MAX_RRPV)
                line_rrpv[off + w]++;
        }
    }
}

void adaptive::train_eviction(long set, long way) {
    size_t idx = set * my_cache->NUM_WAY + way;
    // If a line was never hit, it was streaming/useless. Decrement its PC's score.
    if (!line_outcome[idx]) {
        uint16_t sig = line_sig[idx];
        uint8_t& ctr = shct[sig % SHCT_SIZE];
        if (ctr > 0) ctr--;
    }
}

long adaptive::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set,
                            const champsim::cache_block* current_set,
                            champsim::address ip, champsim::address full_addr,
                            access_type type) {
    check_init();
    for (long w = 0; w < my_cache->NUM_WAY; ++w)
        if (!current_set[w].valid) return w;

    total_evictions++;
    long victim = find_victim_rrip(set);
    train_eviction(set, victim);
    return victim;
}

void adaptive::replacement_cache_fill(uint32_t triggering_cpu, long set, long way,
                                       champsim::address full_addr, champsim::address ip,
                                       champsim::address victim_addr, access_type type) {
    check_init();
    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY) return;

    size_t idx = set * my_cache->NUM_WAY + way;
    uint16_t sig = get_pc_signature(ip);

    if (access_type{type} == access_type::WRITE) {
        line_rrpv[idx] = MAX_RRPV; // Writebacks are always inserted as distant
    } else {
        uint8_t shct_val = shct[sig % SHCT_SIZE];
        if (shct_val >= 5) {
            // High reuse prediction -> insert at RRPV=1
            line_rrpv[idx] = MAX_RRPV - 2; 
            ship_reuse_predictions++;
        } else if (shct_val <= 2) {
            // Streaming prediction -> bypass (RRPV=3), but throttle 1/32 to recover
            if (ship_counter % SHIP_THROTTLE == 0) {
                line_rrpv[idx] = MAX_RRPV - 1; // Give it a chance to get a hit
            } else {
                line_rrpv[idx] = MAX_RRPV;     // Bypass
            }
            ship_counter++;
            ship_streaming_predictions++;
        } else {
            // Neutral -> baseline SRRIP behavior (RRPV=2)
            line_rrpv[idx] = MAX_RRPV - 1; 
            ship_neutral_predictions++;
        }
    }
    line_sig[idx] = sig;
    line_outcome[idx] = false;
}

void adaptive::update_replacement_state(uint32_t triggering_cpu, long set, long way,
                                         champsim::address full_addr, champsim::address ip,
                                         champsim::address victim_addr, access_type type,
                                         uint8_t hit) {
    check_init();
    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY) return;
    if (!hit) return;
    if (access_type{type} == access_type::WRITE) return;

    size_t idx = set * my_cache->NUM_WAY + way;
    line_rrpv[idx] = 0; // Promote to MRU on hit

    // If this is the FIRST hit for this line, train the SHCT (increment)
    if (!line_outcome[idx]) {
        line_outcome[idx] = true;
        uint16_t sig = line_sig[idx];
        uint8_t& ctr = shct[sig % SHCT_SIZE];
        if (ctr < SHCT_MAX) ctr++;
    }
}

void adaptive::replacement_final_stats() {
    std::string name = my_cache->NAME;
    if (name.find("LLC") == std::string::npos) return;

    std::cout << "ADAPTIVE LLC [Pure SHiP w/ Throttle] final stats:" << std::endl;
    std::cout << "ADAPTIVE LLC ship_reuse_predictions: " << ship_reuse_predictions
              << " ship_streaming_predictions: " << ship_streaming_predictions
              << " ship_neutral_predictions: " << ship_neutral_predictions << std::endl;
    std::cout << "ADAPTIVE LLC total_evictions: " << total_evictions << std::endl;
}