#ifndef MPP_H
#define MPP_H

#include <cstdint>
#include <vector>

#include "cache.h"
#include "modules.h"

// ============================================================
//  Multiperspective Perceptron (MPP) LLC Replacement
//
//  Architecture: 2048-set, 16-way LLC
//
//  A lightweight single-layer neural network predicts whether
//  each incoming cache line will be reused before eviction.
//  Five feature tables (perspectives) are indexed by different
//  hash functions of {PC, Page, AccessType, GlobalHistory}.
//  Their weights are summed; the sign of the sum determines
//  insertion RRPV.
//
//  Training is performed on Sampler Sets — a small subset of
//  the cache that stores full metadata (PC, Page, Type) for
//  each line. On a sampler hit, the perceptron is positively
//  reinforced; on sampler eviction without reuse, negatively.
//
//  Metadata budget:
//    RRPV (2-bit packed, 32K lines / 4 per byte) =   8 KB
//    Weight Table 0 (PC)                          =  16 KB
//    Weight Table 1 (Page)                        =  16 KB
//    Weight Table 2 (PC^Page)                     =  16 KB
//    Weight Table 3 (PC^Type)                     =  16 KB
//    Weight Table 4 (PC^History)                  =  16 KB
//    Sampler (128 sets × 16 ways × 12 bytes)      =  24 KB
//    Global History Register                      =   8 B
//    Total                                        = 112 KB ✅
// ============================================================

class mpp : public champsim::modules::replacement {
public:
    explicit mpp(CACHE* cache);

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

    // Weight table parameters
    static constexpr uint32_t NUM_TABLES = 5;
    static constexpr uint32_t TABLE_SIZE = 16384;   // 16K entries per table
    static constexpr uint32_t TABLE_MASK = TABLE_SIZE - 1;
    static constexpr int8_t WEIGHT_MAX = 31;
    static constexpr int8_t WEIGHT_MIN = -32;
    static constexpr int THETA = 10;  // Training threshold

    // Sampler parameters
    static constexpr uint32_t SAMPLER_SETS = 128;
    static constexpr uint32_t SAMPLER_WAYS = 16;

    CACHE* my_cache;

    // Per-line RRPV — 2 bits packed, 4 per byte (8 KB for 32K lines)
    std::vector<uint8_t> rrpv_packed;

    // Perceptron weight tables — 5 × 16K × 1 byte = 80 KB
    std::vector<int8_t> weights;

    // Sampler metadata
    struct SamplerEntry {
        uint32_t tag;       // 4 bytes — address tag for matching
        uint16_t pc_sig;    // 2 bytes — hashed PC signature
        uint16_t page_sig;  // 2 bytes — hashed page signature
        uint8_t type_sig;   // 1 byte  — access type
        uint8_t hist_sig;   // 1 byte  — global history snapshot
        bool valid;         // 1 byte
        bool used;          // 1 byte
        // Total: 12 bytes per entry
    };
    std::vector<SamplerEntry> sampler;      // 128 × 16 × 12 = 24 KB
    std::vector<uint32_t> sampler_set_map;  // maps sampler index → real cache set

    // Global history register (rolling hash of last N PCs)
    uint64_t global_history;

    // Stats
    uint64_t total_evictions;
    uint64_t predicted_reuse;
    uint64_t predicted_dead;
    uint64_t train_positive;
    uint64_t train_negative;

    bool initialized;
    void check_init();

    // RRPV accessors (2-bit packed)
    inline uint8_t get_rrpv(size_t idx) const {
        return (rrpv_packed[idx >> 2] >> ((idx & 3) * 2)) & 0x03;
    }
    inline void set_rrpv(size_t idx, uint8_t val) {
        uint8_t shift = (idx & 3) * 2;
        rrpv_packed[idx >> 2] = static_cast<uint8_t>((rrpv_packed[idx >> 2] & ~(0x03 << shift)) | ((val & 0x03) << shift));
    }

    // Weight table accessors
    inline int8_t& weight(uint32_t table, uint32_t index) {
        return weights[table * TABLE_SIZE + (index & TABLE_MASK)];
    }
    inline int8_t weight(uint32_t table, uint32_t index) const {
        return weights[table * TABLE_SIZE + (index & TABLE_MASK)];
    }

    // Hash functions for 5 perspectives
    uint32_t hash_pc(champsim::address ip) const;
    uint32_t hash_page(champsim::address addr) const;
    uint32_t hash_pc_page(champsim::address ip, champsim::address addr) const;
    uint32_t hash_pc_type(champsim::address ip, access_type type) const;
    uint32_t hash_pc_hist(champsim::address ip) const;

    // Perceptron predict: returns sum of weights
    int predict(champsim::address ip, champsim::address addr, access_type type) const;

    // Perceptron train
    void train(champsim::address ip, champsim::address addr, access_type type,
               uint16_t pc_sig, uint16_t page_sig, uint8_t type_sig, uint8_t hist_sig,
               bool reused);

    // Sampler helpers
    bool is_sampler_set(long set) const;
    uint32_t get_sampler_index(long set) const;

    // RRIP victim selection
    long find_victim_rrip(long set);
};

#endif // MPP_H
