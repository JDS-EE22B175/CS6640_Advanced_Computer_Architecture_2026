#include "srrip.h"
#include <algorithm>
#include <iostream>
#include <string>

srrip::srrip(CACHE* cache)
    : champsim::modules::replacement(cache),
      my_cache(cache),
      total_aging_increments(0),
      total_evictions(0),
      eviction_rrpv_distribution{0} {}

void srrip::check_init() {
    if (rrpv_values.empty())
        rrpv_values.assign(my_cache->NUM_SET * my_cache->NUM_WAY, maxRRPV);
}

long srrip::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set,
                        const champsim::cache_block* current_set, champsim::address ip,
                        champsim::address full_addr, access_type type) {
    check_init();

    // Always prefer an invalid way before evicting a valid line
    for (long way = 0; way < my_cache->NUM_WAY; way++) {
        if (!current_set[way].valid)
            return way;
    }

    long offset = set * my_cache->NUM_WAY;
    auto begin = rrpv_values.begin() + offset;
    auto end   = begin + my_cache->NUM_WAY;

    uint32_t aging_rounds = 0;
    while (true) {
        for (long way = 0; way < my_cache->NUM_WAY; way++) {
            if (*(begin + way) == maxRRPV) {
                uint32_t original_rrpv = (aging_rounds <= maxRRPV) ? (maxRRPV - aging_rounds) : 0;
                eviction_rrpv_distribution[original_rrpv]++;
                total_evictions++;
                total_aging_increments += aging_rounds;
                return way;
            }
        }
        std::for_each(begin, end, [](uint32_t& v) { if (v < maxRRPV) v++; });
        aging_rounds++;
    }
}

void srrip::replacement_cache_fill(uint32_t triggering_cpu, long set, long way,
                                    champsim::address full_addr, champsim::address ip,
                                    champsim::address victim_addr, access_type type) {
    check_init();

    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY)
        return;

    // FIX: Writebacks are dirty lines pushed down from upper levels — they have
    // no demonstrated demand-reuse, so insert them at maxRRPV (immediately evictable).
    // Demand fills and prefetches insert at maxRRPV-1 (standard SRRIP).
    if (access_type{type} == access_type::WRITE)
        rrpv_values[set * my_cache->NUM_WAY + way] = maxRRPV;
    else
        rrpv_values[set * my_cache->NUM_WAY + way] = maxRRPV - 1;
}

void srrip::update_replacement_state(uint32_t triggering_cpu, long set, long way,
                                     champsim::address full_addr, champsim::address ip,
                                     champsim::address victim_addr, access_type type,
                                     uint8_t hit) {
    check_init();

    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY)
        return;

    // Misses are handled in replacement_cache_fill. Here we only handle hits.
    if (!hit) return;

    // On demand hit: predicted to be reused soon -> RRPV=0
    // Skip writeback hits: a writeback hit in the LLC means an upper-level
    // dirty eviction landed on a line that already lives here; not a real
    // reuse signal from the program.
    if (access_type{type} != access_type::WRITE)
        rrpv_values[set * my_cache->NUM_WAY + way] = 0;
}

void srrip::replacement_final_stats() {
    std::string name = my_cache->NAME;
    if (name.find("LLC") == std::string::npos)
        return;

    double avg_aging_rounds = 0.0;
    if (total_evictions > 0)
        avg_aging_rounds = static_cast<double>(total_aging_increments) / static_cast<double>(total_evictions);

    std::cout << "SRRIP LLC eviction_rrpv_dist: 0=" << eviction_rrpv_distribution[0]
              << " 1=" << eviction_rrpv_distribution[1]
              << " 2=" << eviction_rrpv_distribution[2]
              << " 3=" << eviction_rrpv_distribution[3] << std::endl;

    std::cout << "SRRIP LLC avg_aging_rounds: " << avg_aging_rounds << std::endl;
}