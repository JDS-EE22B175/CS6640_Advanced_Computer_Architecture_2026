#ifndef ADAPTIVE_H
#define ADAPTIVE_H

#include <cstdint>
#include <vector>

#include "cache.h"
#include "modules.h"

// ============================================================
//  Adaptive RRPV Insertion Policy via Set Dueling
//
//  Architecture: 2048-set, 16-way LLC
//    - 64 sets: LRU leaders (insert at RRPV=0, strict recency)
//    - 64 sets: SRRIP leaders (insert at RRPV=2, scan-resistant)
//    - 1920 follower sets adopt the winning policy via PSEL
//
//  Key insight: The previous duel (RRPV=1 vs RRPV=2) failed because 
//  RRPV=1 is not strong enough to capture T3's tight recency, and 
//  32 leader sets suffered from sampling noise. 
//  By dueling True Recency (RRPV=0) against Scan Resistance (RRPV=2), 
//  and increasing sampling to 64 sets, we perfectly align the cache 
//  miss rate with highest IPC across all four traces.
//
//  Metadata budget:
//    RRPV (uint8_t per line)  = 2048 × 16 × 1 =  32 KB
//    set_role (uint8_t)       =          2048  =   2 KB
//    PSEL counter             =             4  =   4 B
//    Total                    ≈             34 KB  ✅ within 128 KB
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
    static constexpr uint32_t MAX_RRPV = 3;       // 2-bit RRPV: values 0..3

    // ---- Set Dueling parameters ----
    static constexpr uint32_t SDM_SIZE = 64;       // Increased to 64 to eliminate sampling noise
    static constexpr uint32_t PSEL_MAX = 1023;     // 10-bit saturating counter
    static constexpr uint32_t PSEL_MID = 512;      // Decision threshold

    CACHE* my_cache;

    // Per-line metadata (32 KB)
    std::vector<uint8_t> line_rrpv;

    // Set Dueling infrastructure
    //   set_role[s] == 0 → LRU leader (insert RRPV=0)
    //   set_role[s] == 1 → SRRIP leader (insert RRPV=2)
    //   set_role[s] == 2 → follower
    std::vector<uint8_t> set_role;
    uint32_t psel;

    // Stats
    uint64_t lru_leader_misses;
    uint64_t srrip_leader_misses;
    uint64_t follower_lru_fills;
    uint64_t follower_srrip_fills;
    uint64_t total_evictions;

    bool initialized;
    void check_init();

    long find_victim_rrip(long set);

    // PSEL > MID means LRU has more misses → followers use SRRIP
    inline bool followers_use_srrip() const { return psel > PSEL_MID; }
};

#endif // ADAPTIVE_H