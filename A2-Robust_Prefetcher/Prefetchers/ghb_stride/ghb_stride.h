#ifndef GHB_STRIDE_H
#define GHB_STRIDE_H

#include <cstdint>
#include <optional>
#include <vector>
#include "address.h"
#include "champsim.h"
#include "modules.h"

struct ghb_stride : public champsim::modules::prefetcher {

    struct ghb_entry {
        champsim::block_number cl_addr{};
        int prev_idx  = -1;  // back-link to previous GHB entry for the same PC
        int generation = -1; // set to -1 when the slot is evicted by wrap-around
    };

    struct lookahead_entry {
        champsim::address                    base_address{};
        champsim::block_number::difference_type stride{};
        int current_offset   = 0;
        int degree_remaining = 0;
    };

    static constexpr std::size_t IT_SIZE       = 256;
    static constexpr std::size_t GHB_SIZE      = 512;
    static constexpr int         PREFETCH_DEGREE = 4;
    static constexpr int         LOOKAHEAD      = 1;

    std::vector<int>       index_table;
    std::vector<ghb_entry> ghb;
    int ghb_head          = 0;
    int current_generation = 0;

    std::optional<lookahead_entry> active_lookahead;

public:
    using champsim::modules::prefetcher::prefetcher;

    explicit ghb_stride(CACHE* cache)
        : champsim::modules::prefetcher(cache),
          index_table(IT_SIZE, -1),
          ghb(GHB_SIZE) {}

    uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                      uint8_t cache_hit, bool useful_prefetch,
                                      access_type type, uint32_t metadata_in);

    uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way,
                                   uint8_t prefetch, champsim::address evicted_addr,
                                   uint32_t metadata_in);

    void prefetcher_cycle_operate();

private:
    // A stored GHB index is only valid if the slot still holds the entry that
    // wrote it — i.e. the circular buffer hasn't wrapped around and overwritten
    // it with a newer entry (which would set generation to a different value).
    bool is_valid_ghb_idx(int idx) const {
        if (idx < 0 || idx >= static_cast<int>(GHB_SIZE))
            return false;
        return ghb[idx].generation >= 0;
    }
};

#endif
