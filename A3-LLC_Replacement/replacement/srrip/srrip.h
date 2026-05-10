#ifndef SRRIP_H
#define SRRIP_H

#include <array>
#include <cstdint>
#include <vector>

#include "cache.h"
#include "modules.h"

class srrip : public champsim::modules::replacement {
public:
    explicit srrip(CACHE* cache);

    long find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set,
                     const champsim::cache_block* current_set, champsim::address ip,
                     champsim::address full_addr, access_type type);

    void replacement_cache_fill(uint32_t triggering_cpu, long set, long way,
                                champsim::address full_addr, champsim::address ip,
                                champsim::address victim_addr, access_type type);

    void update_replacement_state(uint32_t triggering_cpu, long set, long way,
                                  champsim::address full_addr, champsim::address ip,
                                  champsim::address victim_addr, access_type type,
                                  uint8_t hit);

    void replacement_final_stats();

private:
    static constexpr uint32_t maxRRPV = 3;  // 2-bit RRPV: values 0..3

    CACHE* my_cache;
    std::vector<uint32_t> rrpv_values;

    // Lazy initialization: NUM_SET / NUM_WAY may not be valid in the constructor
    // depending on ChampSim version, so we size rrpv_values on first use.
    void check_init();

    // Stats
    uint64_t total_aging_increments;
    uint64_t total_evictions;
    std::array<uint64_t, 4> eviction_rrpv_distribution;  // dist by "original" RRPV
};

#endif // SRRIP_H