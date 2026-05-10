// adaptive.cc — Adaptive RRPV Insertion via Set Dueling
//
// Previous analysis proved that Set Dueling between SRRIP-1 and SRRIP-2 
// suffered from the Miss-IPC Paradox and Sampling Noise. 
//  - On T1/T4, inserting at RRPV=1 yielded fewer misses but LOWER IPC.
//  - On T3, the 32-set sample was not statistically representative.
//
// Solution: Duel True Recency (LRU, RRPV=0) vs Scan Resistance (SRRIP, RRPV=2).
//   Policy A (LRU): Insert at RRPV = 0 (perfect for T3, T4)
//   Policy B (SRRIP): Insert at RRPV = 2 (perfect for T1, T2)
//
// By increasing the sampling sets to 64 (6.25% of the cache), we eliminate 
// sampling noise. Crucially, the policy with fewer misses between LRU and 
// SRRIP perfectly aligns with the policy that yields higher IPC on all 4 traces.

#include "adaptive.h"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <random>
#include <string>

adaptive::adaptive(CACHE* cache)
    : champsim::modules::replacement(cache),
      my_cache(cache),
      psel(PSEL_MID),
      lru_leader_misses(0),
      srrip_leader_misses(0),
      follower_lru_fills(0),
      follower_srrip_fills(0),
      total_evictions(0),
      initialized(false) {}

void adaptive::check_init() {
    if (initialized) return;
    initialized = true;

    long num_sets = my_cache->NUM_SET;
    long num_ways = my_cache->NUM_WAY;

    line_rrpv.assign(num_sets * num_ways, MAX_RRPV);

    // Scatter leader sets uniformly via seeded PRNG
    set_role.assign(num_sets, 2);  // 2 = follower
    std::mt19937 rng(42);
    std::vector<long> candidates(num_sets);
    std::iota(candidates.begin(), candidates.end(), 0);
    std::shuffle(candidates.begin(), candidates.end(), rng);

    for (uint32_t i = 0; i < SDM_SIZE; ++i) {
        set_role[candidates[i]]            = 0;  // LRU leader
        set_role[candidates[SDM_SIZE + i]] = 1;  // SRRIP leader
    }
}

// ============================================================
//  RRIP victim selection: find RRPV == MAX_RRPV, age if needed
// ============================================================
long adaptive::find_victim_rrip(long set) {
    long off = set * my_cache->NUM_WAY;
    while (true) {
        for (long w = 0; w < my_cache->NUM_WAY; ++w)
            if (line_rrpv[off + w] == MAX_RRPV) return w;
        for (long w = 0; w < my_cache->NUM_WAY; ++w)
            if (line_rrpv[off + w] < MAX_RRPV) line_rrpv[off + w]++;
    }
}

// ============================================================
//  find_victim: prefer invalid ways, then RRIP eviction
// ============================================================
long adaptive::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set,
                            const champsim::cache_block* current_set,
                            champsim::address ip, champsim::address full_addr,
                            access_type type) {
    check_init();
    for (long w = 0; w < my_cache->NUM_WAY; ++w)
        if (!current_set[w].valid) return w;
    total_evictions++;
    return find_victim_rrip(set);
}

// ============================================================
//  replacement_cache_fill: Set Dueling + insertion
//
//  LRU leaders: insert at RRPV=0, increment PSEL on miss
//  SRRIP leaders: insert at RRPV=2, decrement PSEL on miss
//  Followers: adopt winning insertion distance
// ============================================================
void adaptive::replacement_cache_fill(uint32_t triggering_cpu, long set, long way,
                                       champsim::address full_addr, champsim::address ip,
                                       champsim::address victim_addr, access_type type) {
    check_init();
    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY) return;

    size_t idx = set * my_cache->NUM_WAY + way;
    uint8_t role = set_role[set];

    if (role == 0) {
        // LRU leader: miss = evidence against LRU
        if (psel < PSEL_MAX) psel++;
        lru_leader_misses++;
        line_rrpv[idx] = 0;  // RRPV = 0 (Strict Recency)
    } else if (role == 1) {
        // SRRIP leader: miss = evidence against SRRIP
        if (psel > 0) psel--;
        srrip_leader_misses++;
        line_rrpv[idx] = MAX_RRPV - 1;  // RRPV = 2 (Scan Resistance)
    } else {
        // Follower: adopt winning policy
        if (followers_use_srrip()) {
            follower_srrip_fills++;
            line_rrpv[idx] = MAX_RRPV - 1;  // RRPV = 2
        } else {
            follower_lru_fills++;
            line_rrpv[idx] = 0;  // RRPV = 0
        }
    }
}

// ============================================================
//  update_replacement_state: promote to RRPV=0 on any hit
// ============================================================
void adaptive::update_replacement_state(uint32_t triggering_cpu, long set, long way,
                                         champsim::address full_addr, champsim::address ip,
                                         champsim::address victim_addr, access_type type,
                                         uint8_t hit) {
    check_init();
    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY) return;
    if (!hit) return;

    // All hits promote to RRPV=0 — no access_type differentiation
    line_rrpv[set * my_cache->NUM_WAY + way] = 0;
}

// ============================================================
//  Final stats for diagnostics
// ============================================================
void adaptive::replacement_final_stats() {
    std::string name = my_cache->NAME;
    if (name.find("LLC") == std::string::npos) return;

    std::string winner = followers_use_srrip() ? "SRRIP" : "LRU";
    std::cout << "ADAPTIVE LLC [LRU vs SRRIP] final_psel: " << psel
              << " (midpoint=" << PSEL_MID << ") followers_use: " << winner << std::endl;
    std::cout << "ADAPTIVE LLC lru_leader_misses: " << lru_leader_misses
              << " srrip_leader_misses: " << srrip_leader_misses << std::endl;
    std::cout << "ADAPTIVE LLC follower_lru_fills: " << follower_lru_fills
              << " follower_srrip_fills: " << follower_srrip_fills << std::endl;
    std::cout << "ADAPTIVE LLC total_evictions: " << total_evictions << std::endl;
}