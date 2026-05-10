#ifndef ADAPTIVE_H
#define ADAPTIVE_H

#include <cstdint>
#include <vector>

#include "cache.h"
#include "modules.h"

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
    // ---- Design constants ----
    static constexpr uint32_t MAX_RRPV = 3;        // 2-bit RRPV for SRRIP
    static constexpr uint32_t SDM_SIZE = 32;        // Leader sets per policy
    static constexpr uint32_t NUM_POLICY = 2;       // SRRIP=0, MRU=1
    static constexpr uint32_t PSEL_MAX = 1023;      // 10-bit saturating counter
    static constexpr uint32_t PSEL_MID = 512;       // Decision threshold

    CACHE* my_cache;

    // Per-line metadata: dual-purpose storage
    //   For SRRIP sets: stores RRPV (0..3)
    //   For MRU sets:   stores recency rank (0=most recent, saturates at 15)
    //   For follower sets: stores whichever policy is active
    std::vector<uint8_t> line_meta;   // sized to NUM_SET * NUM_WAY

    // Set dueling infrastructure
    //   set_policy[s] == 0 → SRRIP leader
    //   set_policy[s] == 1 → MRU leader
    //   set_policy[s] == 2 → follower
    std::vector<uint8_t> set_policy;  // sized to NUM_SET
    uint32_t psel;                    // 10-bit saturating counter

    // Stats
    uint64_t srrip_leader_misses;
    uint64_t mru_leader_misses;
    uint64_t follower_srrip_fills;
    uint64_t follower_mru_fills;
    uint64_t total_evictions;

    // Lazy init
    bool initialized;
    void check_init();

    // Policy-specific helpers
    long find_victim_srrip(long set);
    long find_victim_mru(long set, const champsim::cache_block* current_set);
    void fill_srrip(long set, long way, access_type type);
    void fill_mru(long set, long way, access_type type);
    void hit_srrip(long set, long way);
    void hit_mru(long set, long way);

    // MRU touch helper: bump all ranks in set, set touched way to 0
    void mru_touch(long set, long way);

    // Which policy should follower sets use?
    inline bool followers_use_mru() const { return psel > PSEL_MID; }
};

#endif // ADAPTIVE_H