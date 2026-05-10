#ifndef ADAPTIVE_H
#define ADAPTIVE_H

#include <cstdint>
#include <vector>

#include "cache.h"
#include "modules.h"

// ============================================================
//  Signature History-based Insertion Policy (SHiP)
//
//  Architecture: 2048-set, 16-way LLC
//
//  This policy uses pure SHiP (no global Set Dueling). We found that Set 
//  Dueling fundamentally fails on T4 because optimizing for cache miss rate 
//  inverts the IPC (SHiP causes more misses but better MLP/IPC). 
//  By applying SHiP per-PC globally, we adapt to each instruction's behavior.
//
//  Crucially, we introduce a **BIP-like Throttle (1/32)** for streaming 
//  predictions. This allows "false streaming" lines (like in T3 and T2) 
//  to survive long enough to get a hit, correcting their SHCT entries.
//
//  Metadata budget:
//    RRPV (uint8_t per line)    = 2048 × 16 × 1 =  32 KB
//    PC sig (uint16_t per line) = 2048 × 16 × 2 =  64 KB
//    SHCT (16384 × uint8_t)     =                  16 KB
//    Total                      ≈                 112 KB  ✅ within 128 KB
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
    // ---- Design constants ----
    static constexpr uint32_t MAX_RRPV = 3;           // 2-bit RRPV
    static constexpr uint32_t SHIP_THROTTLE = 32;     // 1-in-32 SHiP streaming insertion throttle

    // SHiP constants
    static constexpr uint32_t SHCT_SIZE = 16384;      // Signature History Counter Table entries (16K)
    static constexpr uint8_t  SHCT_MAX = 7;           // 3-bit saturating counter max

    CACHE* my_cache;

    // ---- Per-line metadata ----
    std::vector<uint8_t> line_rrpv;    // sized to NUM_SET * NUM_WAY  (32 KB)
    std::vector<uint16_t> line_sig;    // sized to NUM_SET * NUM_WAY  (64 KB)
    std::vector<bool> line_outcome;    // sized to NUM_SET * NUM_WAY  (~4 KB bitpacked)

    // ---- Global structures ----
    std::vector<uint8_t> shct;         // SHCT_SIZE entries (16 KB)
    uint32_t ship_counter;             // Deterministic counter for throttling

    // ---- Stats ----
    uint64_t total_evictions;
    uint64_t ship_reuse_predictions;
    uint64_t ship_streaming_predictions;
    uint64_t ship_neutral_predictions;

    // ---- Init ----
    bool initialized;
    void check_init();

    // ---- Helpers ----
    long find_victim_rrip(long set);
    uint16_t get_pc_signature(champsim::address ip) const;
    void train_eviction(long set, long way);
};

#endif // ADAPTIVE_H