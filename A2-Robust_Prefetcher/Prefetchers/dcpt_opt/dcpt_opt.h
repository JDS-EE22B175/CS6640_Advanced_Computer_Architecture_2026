#ifndef DCPT_OPT_H
#define DCPT_OPT_H

#include <cstdint>
#include <vector>
#include "address.h"
#include "champsim.h"
#include "modules.h"

struct dcpt_opt : public champsim::modules::prefetcher {

    static constexpr int NUM_ENTRIES = 512;
    static constexpr int HISTORY_LEN = 8;

    static constexpr double MSHR_GATE = 0.75;
    static constexpr int    MAX_DEGREE = 4;

    // Confidence thresholds: PREF to start issuing, HIT to issue on cache hits,
    // FULL to unlock the maximum degree.
    static constexpr int CONF_MAX  = 7;
    static constexpr int CONF_PREF = 2;
    static constexpr int CONF_HIT  = 5;
    static constexpr int CONF_FULL = 6;

    struct entry {
        uint64_t pc_tag    = 0;
        uint64_t last_block = 0;

        int64_t deltas[HISTORY_LEN] = {};
        int     head     = 0;
        int     conf     = 0;
        int     lru_age  = 0;
        bool    valid    = false;

        // Lightweight stride detector layered on top of DCPT.
        // When the same delta repeats twice it short-circuits the full
        // delta-correlation search and issues a simple stride prefetch.
        int64_t last_delta  = 0;
        int     stride_conf = 0;
        int64_t stride      = 0;
    };

    std::vector<entry> table;

    using champsim::modules::prefetcher::prefetcher;

    explicit dcpt_opt(CACHE* cache)
        : champsim::modules::prefetcher(cache),
          table(NUM_ENTRIES) {}

    uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                      uint8_t cache_hit, bool useful_prefetch,
                                      access_type type, uint32_t metadata_in);

    uint32_t prefetcher_cache_fill(champsim::address, long, long,
                                   uint8_t, champsim::address,
                                   uint32_t metadata_in) { return metadata_in; }

    void prefetcher_cycle_operate() {}

private:
    int  find_entry(uint64_t pc);
    int  allocate_entry(uint64_t pc, uint64_t block);
    void age_lru();

    int  degree_for_conf(int conf, double mshr) const;

    void do_prefetch_stride(entry& e, uint64_t block,
                            champsim::address addr,
                            uint32_t metadata_in, int degree);

    void do_prefetch_dcpt(entry& e, int match_idx, uint64_t block,
                          champsim::address addr,
                          uint32_t metadata_in, int degree);
};

#endif
