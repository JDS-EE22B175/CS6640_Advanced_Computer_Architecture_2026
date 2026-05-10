#include "adaptive.h"
#include <algorithm>
#include <iostream>
#include <string>

// DRRIP: Dynamic Re-Reference Interval Prediction
//
// Two competing insertion policies dueled via set sampling:
//
//   SRRIP: always insert at RRPV = maxRRPV-1 = 2
//          Good for workloads with reuse — lines get a chance before eviction
//
//   BRRIP: insert at RRPV = maxRRPV = 3 with probability (BRRIP_FREQ-1)/BRRIP_FREQ
//          insert at RRPV = maxRRPV-1 = 2 with probability 1/BRRIP_FREQ
//          Good for streaming — most lines evicted immediately, occasional
//          lines given a chance (prevents total cache thrash)
//
// NUM_LEADER_SETS sets per policy are permanently assigned.
// A 10-bit PSEL counter tracks which policy gets more misses.
// All remaining (follower) sets use whichever policy PSEL currently favors.
//
// Eviction in both policies: identical SRRIP scan (find RRPV=3, age if none found).
// Only the insertion RRPV differs between policies.

adaptive::adaptive(CACHE* cache)
    : champsim::modules::replacement(cache),
      my_cache(cache),
      psel(PSEL_INIT),
      total_evictions(0),
      srrip_leader_misses(0),
      brrip_leader_misses(0) {}

void adaptive::check_init() {
    if (!rrpv_values.empty())
        return;

    long num_sets = my_cache->NUM_SET;
    long num_ways = my_cache->NUM_WAY;

    rrpv_values.assign(num_sets * num_ways, ADAPT_maxRRPV);
    set_type.assign(num_sets, 2); // default: follower
    brrip_counter.assign(num_sets, 0);

    // Assign leader sets evenly across the set index space.
    // Sets where (set % (num_sets / NUM_LEADER_SETS)) == 0 → SRRIP leader
    // Sets where (set % (num_sets / NUM_LEADER_SETS)) == 1 → BRRIP leader
    // This ensures leaders are spread across the cache, not clustered.
    long stride = std::max(1L, num_sets / static_cast<long>(NUM_LEADER_SETS));
    for (long s = 0; s < num_sets; s++) {
        long mod = s % stride;
        if (mod == 0)
            set_type[s] = 0; // SRRIP leader
        else if (mod == 1)
            set_type[s] = 1; // BRRIP leader
        // else: follower (already set)
    }
}

long adaptive::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set,
                            const champsim::cache_block* current_set, champsim::address ip,
                            champsim::address full_addr, access_type type) {
    check_init();

    for (long way = 0; way < my_cache->NUM_WAY; way++) {
        if (!current_set[way].valid)
            return way;
    }

    long offset = set * my_cache->NUM_WAY;
    auto begin = rrpv_values.begin() + offset;
    auto end   = begin + my_cache->NUM_WAY;

    // Standard SRRIP eviction scan — identical for all sets
    while (true) {
        for (long way = 0; way < my_cache->NUM_WAY; way++) {
            if (*(begin + way) == ADAPT_maxRRPV) {
                total_evictions++;
                return way;
            }
        }
        std::for_each(begin, end, [](uint32_t& v) { if (v < ADAPT_maxRRPV) v++; });
    }
}

void adaptive::replacement_cache_fill(uint32_t triggering_cpu, long set, long way,
                                       champsim::address full_addr, champsim::address ip,
                                       champsim::address victim_addr, access_type type) {
    check_init();

    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY)
        return;

    // Writebacks insert at maxRRPV regardless of policy — immediately evictable,
    // don't pollute the cache with dirty lines that may not be reused
    if (access_type{type} == access_type::WRITE) {
        rrpv_values[set * my_cache->NUM_WAY + way] = ADAPT_maxRRPV;
        return;
    }

    // Determine which policy this set uses
    uint8_t stype = set_type[set];
    bool use_srrip;

    if (stype == 0) {
        use_srrip = true;  // SRRIP leader
    } else if (stype == 1) {
        use_srrip = false; // BRRIP leader
    } else {
        // Follower: use whichever policy PSEL currently favors
        use_srrip = (psel < PSEL_INIT);
    }

    uint32_t insert_rrpv;
    if (use_srrip) {
        // SRRIP: always insert at RRPV=2
        insert_rrpv = ADAPT_maxRRPV - 1;
    } else {
        // BRRIP: insert at RRPV=3 most of the time, RRPV=2 occasionally (1/BRRIP_FREQ)
        // The occasional RRPV=2 insertion prevents complete cache bypass on workloads
        // with some reuse — without it, BRRIP degenerates to always-evict behavior
        brrip_counter[set] = (brrip_counter[set] + 1) % BRRIP_FREQ;
        insert_rrpv = (brrip_counter[set] == 0) ? (ADAPT_maxRRPV - 1) : ADAPT_maxRRPV;
    }

    rrpv_values[set * my_cache->NUM_WAY + way] = insert_rrpv;

    // Update PSEL on miss (this is a fill, meaning a prior miss occurred)
    // Miss in SRRIP leader → BRRIP might be better → increment PSEL toward BRRIP
    // Miss in BRRIP leader → SRRIP might be better → decrement PSEL toward SRRIP
    if (stype == 0 && psel < PSEL_MAX) {
        psel++;
        srrip_leader_misses++;
    } else if (stype == 1 && psel > 0) {
        psel--;
        brrip_leader_misses++;
    }
}

void adaptive::update_replacement_state(uint32_t triggering_cpu, long set, long way,
                                         champsim::address full_addr, champsim::address ip,
                                         champsim::address victim_addr, access_type type,
                                         uint8_t hit) {
    check_init();

    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY)
        return;

    // On demand hit: set RRPV=0 — predicted to be reused very soon
    // Skip writeback hits — they don't indicate genuine reuse
    if (hit && access_type{type} != access_type::WRITE)
        rrpv_values[set * my_cache->NUM_WAY + way] = 0;
}

void adaptive::replacement_final_stats() {
    std::string name = my_cache->NAME;
    if (name.find("LLC") == std::string::npos)
        return;

    const char* final_policy = (psel < PSEL_INIT) ? "SRRIP" : "BRRIP";
    double srrip_miss_frac = 0.0;
    uint64_t total_leader_misses = srrip_leader_misses + brrip_leader_misses;
    if (total_leader_misses > 0)
        srrip_miss_frac = static_cast<double>(srrip_leader_misses) /
                          static_cast<double>(total_leader_misses);

    std::cout << "ADAPTIVE LLC final_policy: " << final_policy
              << " psel=" << psel << std::endl;
    std::cout << "ADAPTIVE LLC srrip_leader_miss_frac: " << srrip_miss_frac
              << " (" << srrip_leader_misses << " vs " << brrip_leader_misses << ")" << std::endl;
    std::cout << "ADAPTIVE LLC total_evictions: " << total_evictions << std::endl;
}