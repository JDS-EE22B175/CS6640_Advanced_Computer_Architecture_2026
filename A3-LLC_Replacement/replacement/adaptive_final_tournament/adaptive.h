#ifndef ADAPTIVE_H
#define ADAPTIVE_H

#include <cstdint>
#include <vector>

#include "cache.h"
#include "modules.h"

// ============================================================
//  Tri-Mode Tournament Adaptive Policy (LRU vs SRRIP vs SHiP)
//
//  Maintains perfect parallel state for THREE algorithms:
//  1. True LRU (4-bit age)
//  2. True SRRIP (2-bit RRPV)
//  3. SHiP (2-bit RRPV + 14-bit Signature + 3-bit Counters)
//
//  Hardware Budget:
//  - line_meta: 8 bits per line (4 LRU + 2 SRRIP + 2 SHiP) = 32KB
//  - line_sig: 16 bits per line (15-bit PC sig + 1-bit Reused) = 64KB
//  - SHCT: 16K entries x 1 byte = 16KB
//  - set_role: 1 byte x 2K sets = 2KB
//  - 3x 16-bit Miss Counters = 6 Bytes
//  Total: 114 KB (Limit: 128 KB) ✅
// ============================================================

class adaptive : public champsim::modules::replacement {
public:
    explicit adaptive(CACHE* cache);

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
    static constexpr uint32_t MAX_RRPV = 3;
    static constexpr uint32_t SDM_SIZE = 24;
    static constexpr uint32_t SHCT_SIZE = 16384;
    static constexpr uint32_t SIG_MASK = 0x3FFF; // 14 bits

    CACHE* my_cache;

    std::vector<uint8_t> line_meta; 
    std::vector<uint16_t> line_sig;
    std::vector<uint8_t> shct;
    std::vector<uint8_t> set_role;

    uint32_t misses_lru;
    uint32_t misses_srrip;
    uint32_t misses_ship;
    uint64_t total_evictions;

    bool initialized;
    void check_init();

    inline uint16_t get_sig(champsim::address ip) const {
        uint64_t pc = ip.to<uint64_t>();
        return static_cast<uint16_t>((pc ^ (pc >> 14)) & SIG_MASK);
    }
};

#endif // ADAPTIVE_H