// adaptive.cc — Tri-Mode Tournament Adaptive Replacement (LRU vs SRRIP vs SHiP)
//
// Fully utilizes the 128KB hardware budget to track three algorithms simultaneously
// using bit-packed metadata. Leader sets sample each algorithm exactly, and 
// followers dynamically adopt the one with the lowest decay-adjusted miss count.

#include "adaptive.h"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <random>

adaptive::adaptive(CACHE* cache)
    : champsim::modules::replacement(cache),
      my_cache(cache),
      misses_lru(0),
      misses_srrip(0),
      misses_ship(0),
      total_evictions(0),
      initialized(false) {}

void adaptive::check_init() {
    if (initialized) return;
    initialized = true;

    long num_sets = my_cache->NUM_SET;
    long num_ways = my_cache->NUM_WAY;
    long total_lines = num_sets * num_ways;

    line_meta.assign(total_lines, 0);
    line_sig.assign(total_lines, 0);
    shct.assign(SHCT_SIZE, 2);  // warm start: reuse-friendly for T1/T3

    for (long s = 0; s < num_sets; ++s) {
        for (long w = 0; w < num_ways; ++w) {
            uint8_t lru_age = static_cast<uint8_t>(w);
            uint8_t srrip_rrpv = MAX_RRPV;
            uint8_t ship_rrpv = MAX_RRPV;
            line_meta[s * num_ways + w] = (lru_age & 0x0F) | (srrip_rrpv << 4) | (ship_rrpv << 6);
        }
    }

    set_role.assign(num_sets, 3); // 3 = follower
    std::mt19937 rng(42);
    std::vector<long> candidates(num_sets);
    std::iota(candidates.begin(), candidates.end(), 0);
    std::shuffle(candidates.begin(), candidates.end(), rng);

    for (uint32_t i = 0; i < SDM_SIZE; ++i) {
        set_role[candidates[i]]               = 0;  // LRU leader
        set_role[candidates[SDM_SIZE + i]]    = 1;  // SRRIP leader
        set_role[candidates[SDM_SIZE * 2 + i]] = 2;  // SHiP leader
    }
}

long adaptive::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set,
                            const champsim::cache_block* current_set,
                            champsim::address ip, champsim::address full_addr,
                            access_type type) {
    check_init();

    for (long w = 0; w < my_cache->NUM_WAY; ++w) {
        if (!current_set[w].valid) return w;
    }

    total_evictions++;
    
    uint8_t role = set_role[set];
    int best_policy = 0;
    
    if (role == 3) {
        uint32_t min_misses = misses_lru;
        best_policy = 0;
        if (misses_srrip < min_misses) { best_policy = 1; min_misses = misses_srrip; }
        if (misses_ship < min_misses)  { best_policy = 2; }
    } else {
        best_policy = role;
    }

    long offset = set * my_cache->NUM_WAY;

    if (best_policy == 0) {
        // LRU
        uint8_t target_age = static_cast<uint8_t>(my_cache->NUM_WAY - 1);
        for (long w = 0; w < my_cache->NUM_WAY; ++w) {
            if ((line_meta[offset + w] & 0x0F) == target_age) return w;
        }
    } else if (best_policy == 1) {
        // SRRIP
        while (true) {
            for (long w = 0; w < my_cache->NUM_WAY; ++w) {
                if (((line_meta[offset + w] >> 4) & 0x03) == MAX_RRPV) return w;
            }
            for (long w = 0; w < my_cache->NUM_WAY; ++w) {
                uint8_t m = line_meta[offset + w];
                uint8_t r = (m >> 4) & 0x03;
                if (r < MAX_RRPV) line_meta[offset + w] = static_cast<uint8_t>((m & 0xCF) | ((r + 1) << 4));
            }
        }
    } else if (best_policy == 2) {
        // SHiP
        while (true) {
            for (long w = 0; w < my_cache->NUM_WAY; ++w) {
                if (((line_meta[offset + w] >> 6) & 0x03) == MAX_RRPV) return w;
            }
            for (long w = 0; w < my_cache->NUM_WAY; ++w) {
                uint8_t m = line_meta[offset + w];
                uint8_t r = (m >> 6) & 0x03;
                if (r < MAX_RRPV) line_meta[offset + w] = static_cast<uint8_t>((m & 0x3F) | ((r + 1) << 6));
            }
        }
    }
    return 0;
}

void adaptive::replacement_cache_fill(uint32_t triggering_cpu, long set, long way,
                                       champsim::address full_addr, champsim::address ip,
                                       champsim::address victim_addr, access_type type) {
    check_init();
    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY) return;

    uint8_t role = set_role[set];
    
    if (role == 0) misses_lru++;
    else if (role == 1) misses_srrip++;
    else if (role == 2) misses_ship++;

    if (misses_lru > 32000 || misses_srrip > 32000 || misses_ship > 32000) {
        misses_lru >>= 1; misses_srrip >>= 1; misses_ship >>= 1;
    }

    size_t idx = set * my_cache->NUM_WAY + way;
    long offset = set * my_cache->NUM_WAY;

    // Train SHiP on global eviction if unreused
    uint16_t evicted_sig_full = line_sig[idx];
    if ((evicted_sig_full & 0x8000) == 0) {
        uint16_t evicted_sig = evicted_sig_full & 0x7FFF;
        if (shct[evicted_sig] > 0) shct[evicted_sig]--;
    }

    uint8_t victim_meta = line_meta[idx];
    uint8_t victim_age = victim_meta & 0x0F;

    // LRU age updates
    for (long w = 0; w < my_cache->NUM_WAY; ++w) {
        if (w != way) {
            uint8_t m = line_meta[offset + w];
            uint8_t a = m & 0x0F;
            if (a < victim_age) {
                line_meta[offset + w] = static_cast<uint8_t>((m & 0xF0) | (a + 1));
            }
        }
    }

    uint8_t new_srrip = (access_type{type} == access_type::WRITE) ? MAX_RRPV : (MAX_RRPV - 1);
    uint16_t new_sig = get_sig(ip);
    uint8_t new_ship = 0;
    
    if (access_type{type} == access_type::WRITE) {
        new_ship = MAX_RRPV;
    } else {
        new_ship = (shct[new_sig] == 0) ? (MAX_RRPV - 1) : 0;
    }

    line_meta[idx] = static_cast<uint8_t>((0) | (new_srrip << 4) | (new_ship << 6));
    line_sig[idx] = new_sig; // Reused bit is 0
}

void adaptive::update_replacement_state(uint32_t triggering_cpu, long set, long way,
                                         champsim::address full_addr, champsim::address ip,
                                         champsim::address victim_addr, access_type type,
                                         uint8_t hit) {
    check_init();
    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY) return;
    if (!hit) return;

    if (access_type{type} != access_type::WRITE) {
        size_t idx = set * my_cache->NUM_WAY + way;
        long offset = set * my_cache->NUM_WAY;

        uint16_t sig_full = line_sig[idx];
        if ((sig_full & 0x8000) == 0) {
            line_sig[idx] |= 0x8000;
            uint16_t sig = sig_full & 0x7FFF;
            if (shct[sig] < 7) shct[sig]++;
        }

        uint8_t hit_meta = line_meta[idx];
        uint8_t hit_age = hit_meta & 0x0F;

        for (long w = 0; w < my_cache->NUM_WAY; ++w) {
            uint8_t m = line_meta[offset + w];
            uint8_t a = m & 0x0F;
            if (a < hit_age) {
                line_meta[offset + w] = static_cast<uint8_t>((m & 0xF0) | (a + 1));
            }
        }

        line_meta[idx] = 0; // MRU for all 3 policies
    }
}

void adaptive::replacement_final_stats() {
    std::string name = my_cache->NAME;
    if (name.find("LLC") == std::string::npos) return;

    std::string winner = "LRU";
    uint32_t min_misses = misses_lru;
    if (misses_srrip < min_misses) { winner = "SRRIP"; min_misses = misses_srrip; }
    if (misses_ship < min_misses)  { winner = "SHiP"; }

    std::cout << "ADAPTIVE LLC [Tri-Mode Tournament] follower_preferred: " << winner << std::endl;
    std::cout << "ADAPTIVE LLC misses_lru: " << misses_lru
              << " misses_srrip: " << misses_srrip 
              << " misses_ship: " << misses_ship << std::endl;
    std::cout << "ADAPTIVE LLC total_evictions: " << total_evictions << std::endl;
}