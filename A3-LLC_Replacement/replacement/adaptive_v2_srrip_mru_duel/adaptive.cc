// adaptive.cc - Set Dueling Adaptive Policy: SRRIP vs MRU
//
// Architecture: 2048-set, 16-way LLC
//   - 32 sets dedicated as SRRIP leaders
//   - 32 sets dedicated as MRU leaders
//   - 1984 follower sets adopt the winning policy via a 10-bit PSEL counter
//
// SRRIP component: 2-bit RRPV, insert at maxRRPV-1, promote to 0 on hit
// MRU component: 4-bit recency rank, evict smallest rank (most recent)
//
// Writebacks: insert at maxRRPV (SRRIP) or rank 0 (MRU) — no special treatment.
// This avoids the writeback-bypass thrashing observed in prior experiments.

#include "adaptive.h"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <random>
#include <string>

adaptive::adaptive(CACHE* cache)
    : champsim::modules::replacement(cache),
      my_cache(cache),
      psel(PSEL_MID),  // Start at midpoint — no initial bias
      srrip_leader_misses(0),
      mru_leader_misses(0),
      follower_srrip_fills(0),
      follower_mru_fills(0),
      total_evictions(0),
      initialized(false) {}

void adaptive::check_init() {
    if (initialized) return;
    initialized = true;

    long num_sets = my_cache->NUM_SET;
    long num_ways = my_cache->NUM_WAY;
    long N = num_sets * num_ways;

    // Per-line metadata: initialized to MAX_RRPV (SRRIP default: distant)
    line_meta.assign(N, MAX_RRPV);

    // Assign leader sets using a deterministic PRNG for reproducibility.
    // This scatters leaders uniformly across the set space to avoid bias.
    set_policy.assign(num_sets, 2);  // 2 = follower by default

    // Generate SDM_SIZE * NUM_POLICY = 64 unique random set indices
    std::mt19937 rng(42);  // fixed seed for reproducibility
    std::vector<long> candidates(num_sets);
    std::iota(candidates.begin(), candidates.end(), 0);
    std::shuffle(candidates.begin(), candidates.end(), rng);

    for (uint32_t i = 0; i < SDM_SIZE; ++i) {
        set_policy[candidates[i]] = 0;                // SRRIP leader
        set_policy[candidates[SDM_SIZE + i]] = 1;     // MRU leader
    }

    // Initialize MRU leader sets: all ways start at rank 0 (will be properly
    // set during fills). SRRIP leaders already have MAX_RRPV from line_meta init.
}

// ============================================================
//  SRRIP helpers
// ============================================================

long adaptive::find_victim_srrip(long set) {
    long off = set * my_cache->NUM_WAY;

    // Search for a line with RRPV == MAX_RRPV
    while (true) {
        for (long w = 0; w < my_cache->NUM_WAY; ++w) {
            if (line_meta[off + w] == MAX_RRPV)
                return w;
        }
        // Age all lines in this set
        for (long w = 0; w < my_cache->NUM_WAY; ++w) {
            if (line_meta[off + w] < MAX_RRPV)
                line_meta[off + w]++;
        }
    }
}

void adaptive::fill_srrip(long set, long way, access_type type) {
    size_t idx = set * my_cache->NUM_WAY + way;
    auto t = access_type{type};
    if (t == access_type::WRITE) {
        line_meta[idx] = MAX_RRPV;      // Writeback → immediately evictable
    } else {
        line_meta[idx] = MAX_RRPV - 1;  // Demand/prefetch → standard SRRIP insert
    }
}

void adaptive::hit_srrip(long set, long way) {
    // Hit Priority (HP): promote to RRPV=0
    line_meta[set * my_cache->NUM_WAY + way] = 0;
}

// ============================================================
//  MRU helpers
// ============================================================

void adaptive::mru_touch(long set, long way) {
    size_t base = static_cast<size_t>(set) * my_cache->NUM_WAY;
    for (long i = 0; i < my_cache->NUM_WAY; ++i) {
        auto& rank = line_meta[base + i];
        if (rank < 15) rank++;  // Age all lines, saturate at 15
    }
    line_meta[base + way] = 0;  // Touched way becomes most recent
}

long adaptive::find_victim_mru(long set, const champsim::cache_block* current_set) {
    size_t base = static_cast<size_t>(set) * my_cache->NUM_WAY;

    // Evict the way with the smallest rank (most recently used).
    // Ties broken by lowest way index.
    long victim = 0;
    uint8_t min_rank = line_meta[base];
    for (long w = 1; w < my_cache->NUM_WAY; ++w) {
        if (line_meta[base + w] < min_rank) {
            min_rank = line_meta[base + w];
            victim = w;
        }
    }
    return victim;
}

void adaptive::fill_mru(long set, long way, access_type type) {
    auto t = access_type{type};
    if (t == access_type::LOAD || t == access_type::RFO) {
        mru_touch(set, way);
    } else {
        // Writebacks/prefetches: rank 0 → evicted next under MRU
        size_t base = static_cast<size_t>(set) * my_cache->NUM_WAY;
        line_meta[base + way] = 0;
    }
}

void adaptive::hit_mru(long set, long way) {
    mru_touch(set, way);
}

// ============================================================
//  Main replacement interface
// ============================================================

long adaptive::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set,
                            const champsim::cache_block* current_set,
                            champsim::address ip, champsim::address full_addr,
                            access_type type) {
    check_init();

    // Always prefer an invalid way
    for (long w = 0; w < my_cache->NUM_WAY; ++w)
        if (!current_set[w].valid) return w;

    total_evictions++;

    uint8_t policy = set_policy[set];

    if (policy == 0) {
        // SRRIP leader
        return find_victim_srrip(set);
    } else if (policy == 1) {
        // MRU leader
        return find_victim_mru(set, current_set);
    } else {
        // Follower: use the winning policy
        if (followers_use_mru())
            return find_victim_mru(set, current_set);
        else
            return find_victim_srrip(set);
    }
}

void adaptive::replacement_cache_fill(uint32_t triggering_cpu, long set, long way,
                                       champsim::address full_addr, champsim::address ip,
                                       champsim::address victim_addr, access_type type) {
    check_init();
    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY) return;

    uint8_t policy = set_policy[set];

    if (policy == 0) {
        // SRRIP leader: miss here → increment PSEL (evidence against SRRIP)
        if (psel < PSEL_MAX) psel++;
        srrip_leader_misses++;
        fill_srrip(set, way, type);
    } else if (policy == 1) {
        // MRU leader: miss here → decrement PSEL (evidence against MRU)
        if (psel > 0) psel--;
        mru_leader_misses++;
        fill_mru(set, way, type);
    } else {
        // Follower
        if (followers_use_mru()) {
            follower_mru_fills++;
            fill_mru(set, way, type);
        } else {
            follower_srrip_fills++;
            fill_srrip(set, way, type);
        }
    }
}

void adaptive::update_replacement_state(uint32_t triggering_cpu, long set, long way,
                                         champsim::address full_addr, champsim::address ip,
                                         champsim::address victim_addr, access_type type,
                                         uint8_t hit) {
    check_init();
    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY) return;
    if (!hit) return;  // Misses handled in replacement_cache_fill
    if (access_type{type} == access_type::WRITE) return;  // Ignore writeback hits

    uint8_t policy = set_policy[set];

    if (policy == 0) {
        hit_srrip(set, way);
    } else if (policy == 1) {
        hit_mru(set, way);
    } else {
        // Follower: use the current winning policy's hit logic
        if (followers_use_mru())
            hit_mru(set, way);
        else
            hit_srrip(set, way);
    }
}

void adaptive::replacement_final_stats() {
    std::string name = my_cache->NAME;
    if (name.find("LLC") == std::string::npos) return;

    std::string chosen = followers_use_mru() ? "MRU" : "SRRIP";
    std::cout << "ADAPTIVE LLC final_psel: " << psel
              << " (midpoint=" << PSEL_MID << ") → followers_use: " << chosen << std::endl;
    std::cout << "ADAPTIVE LLC srrip_leader_misses: " << srrip_leader_misses
              << " mru_leader_misses: " << mru_leader_misses << std::endl;
    std::cout << "ADAPTIVE LLC follower_srrip_fills: " << follower_srrip_fills
              << " follower_mru_fills: " << follower_mru_fills << std::endl;
    std::cout << "ADAPTIVE LLC total_evictions: " << total_evictions << std::endl;
}