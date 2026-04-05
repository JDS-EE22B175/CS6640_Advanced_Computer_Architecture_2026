#ifndef BOP_ULTIMATE_H
#define BOP_ULTIMATE_H

#include <cstdint>
#include <array>
#include "champsim.h"
#include "modules.h"

struct bop_ultimate : public champsim::modules::prefetcher {

    // --- Core Parameters ---
    static constexpr std::size_t RR_CAPACITY  = 256;
    static constexpr int         PHASE_LENGTH = 256;
    static constexpr int         MIN_HITS     = PHASE_LENGTH / 4;

    // Spatial offsets mapping common struct/array strides
    static constexpr std::array<int, 16> test_offsets = {
        1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 8, -8, 16, -16
    };

    // --- State Trackers ---
    std::array<int, 16>       offset_scores{};
    std::array<uint64_t, 256> history_ring{};

    int ring_ptr      = 0;
    int phase_counter = 0;
    int active_offset = 1;
    int issue_depth   = 1;

public:
    using champsim::modules::prefetcher::prefetcher;

    uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                      uint8_t cache_hit, bool useful_prefetch,
                                      access_type type, uint32_t metadata_in);
    void prefetcher_cycle_operate() {}
};

#endif