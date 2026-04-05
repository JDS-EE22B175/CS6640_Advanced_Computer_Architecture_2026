#ifndef HASH_BOP_H
#define HASH_BOP_H

#include <cstdint>
#include <array>
#include "champsim.h"
#include "modules.h"

struct hash_bop : public champsim::modules::prefetcher {

    static constexpr int TABLE_SIZE = 256;
    static constexpr int PHASE_MAX = 256;
    static constexpr int THRESHOLD = PHASE_MAX / 4; // require 25% hit rate

    // testing 20 common offsets
    static constexpr std::array<int, 20> offsets = {
        1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 
        6, -6, 8, -8, 10, -10, 12, -12, 16, -16
    };

    std::array<uint64_t, TABLE_SIZE> history_table{};
    std::array<int, 20> scores{};

    int timer = 0;
    int best_offset = 1;
    int pref_degree = 1;

    // simple hash to map 64-bit address to an 8-bit index
    inline uint32_t hash_addr(uint64_t blk) const {
        return (blk ^ (blk >> 8) ^ (blk >> 16)) & 0xFF;
    }

public:
    using champsim::modules::prefetcher::prefetcher;

    uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                      uint8_t cache_hit, bool useful_prefetch,
                                      access_type type, uint32_t metadata_in);
    void prefetcher_cycle_operate() {}
};

#endif