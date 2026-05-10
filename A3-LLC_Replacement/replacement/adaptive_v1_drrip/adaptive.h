#ifndef ADAPTIVE_H
#define ADAPTIVE_H

#include "cache.h"
#include "modules.h"
#include <vector>
#include <cstdint>
#include <string>

// DRRIP: Dynamic Re-Reference Interval Prediction
// Set dueling between SRRIP (insert at RRPV=2) and BRRIP (insert at RRPV=3).
// Follower sets use whichever policy has fewer misses, tracked by PSEL counter.
// Reference: Jaleel et al., ISCA 2010.

static constexpr uint32_t ADAPT_maxRRPV    = 3;
static constexpr uint32_t NUM_LEADER_SETS  = 32;  // leader sets per policy
static constexpr uint32_t PSEL_MAX         = 1023; // 10-bit saturating counter
static constexpr uint32_t PSEL_INIT        = 512;  // neutral starting point
static constexpr uint32_t BRRIP_FREQ       = 32;   // insert at RRPV=2 1/32 of the time in BRRIP

struct adaptive : champsim::modules::replacement {
    CACHE* my_cache;
    std::vector<uint32_t> rrpv_values;

    // Set Dueling selector: <512 → use SRRIP, >=512 → use BRRIP
    uint32_t psel;

    // Per-set policy assignment: 0=SRRIP leader, 1=BRRIP leader, 2=follower
    std::vector<uint8_t> set_type;

    // BRRIP epsilon counter per set (for the 1/32 SRRIP insertion in BRRIP sets)
    std::vector<uint32_t> brrip_counter;

    // Stats
    uint64_t total_evictions;
    uint64_t srrip_leader_misses;
    uint64_t brrip_leader_misses;

    explicit adaptive(CACHE* cache);
    void check_init();

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
};

#endif