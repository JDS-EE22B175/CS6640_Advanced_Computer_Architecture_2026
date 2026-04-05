#include "ghb_stride.h"
#include "cache.h"

uint32_t ghb_stride::prefetcher_cache_operate(
    champsim::address addr, champsim::address ip,
    uint8_t cache_hit, bool useful_prefetch,
    access_type type, uint32_t metadata_in)
{
    champsim::block_number cl_addr{addr};

    using namespace champsim::data::data_literals;
    uint64_t ip_hash = ip.slice_upper<2_b>().to<uint64_t>();
    int it_idx = static_cast<int>(ip_hash % IT_SIZE);

    int prev_idx = index_table[it_idx];
    if (!is_valid_ghb_idx(prev_idx))
        prev_idx = -1;

    // Invalidate the slot we're about to evict so that any Index Table pointer
    // still referencing it will be treated as stale by is_valid_ghb_idx().
    ghb[ghb_head].generation = -1;

    ghb[ghb_head].cl_addr    = cl_addr;
    ghb[ghb_head].prev_idx   = prev_idx;
    ghb[ghb_head].generation = current_generation++;

    index_table[it_idx] = ghb_head;

    // We need three consecutive accesses from the same PC to compute two strides
    // and confirm they match.
    if (prev_idx != -1) {
        int older_idx = ghb[prev_idx].prev_idx;
        if (!is_valid_ghb_idx(older_idx))
            older_idx = -1;

        if (older_idx != -1) {
            auto stride1 = champsim::offset(ghb[prev_idx].cl_addr, cl_addr);
            auto stride2 = champsim::offset(ghb[older_idx].cl_addr, ghb[prev_idx].cl_addr);

            if (stride1 != 0 && stride1 == stride2)
                active_lookahead = {champsim::address{cl_addr}, stride1, LOOKAHEAD, PREFETCH_DEGREE};
        }
    }

    ghb_head = (ghb_head + 1) % static_cast<int>(GHB_SIZE);
    return metadata_in;
}

void ghb_stride::prefetcher_cycle_operate()
{
    if (!active_lookahead.has_value())
        return;

    auto& state = active_lookahead.value();
    assert(state.degree_remaining > 0);

    // Arithmetic in block_number space keeps the offset in cache-line units.
    // Adding directly to a champsim::address would offset in bytes, which is wrong.
    champsim::block_number base_block{state.base_address};
    champsim::block_number pf_block = base_block + (state.stride * state.current_offset);
    champsim::address      pf_address{pf_block};

    if (!intern_->virtual_prefetch &&
        champsim::page_number{pf_address} != champsim::page_number{state.base_address}) {
        active_lookahead.reset();
        return;
    }

    const bool fill_this_level = intern_->get_mshr_occupancy_ratio() < 0.5;
    const bool success = prefetch_line(pf_address, fill_this_level, 0);

    if (success) {
        state.current_offset++;
        state.degree_remaining--;
        if (state.degree_remaining == 0)
            active_lookahead.reset();
    }
}

uint32_t ghb_stride::prefetcher_cache_fill(
    champsim::address addr, long set, long way,
    uint8_t prefetch, champsim::address evicted_addr,
    uint32_t metadata_in)
{
    return metadata_in;
}
