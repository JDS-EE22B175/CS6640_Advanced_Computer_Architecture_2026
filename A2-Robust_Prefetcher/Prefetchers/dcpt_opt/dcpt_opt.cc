#include "dcpt_opt.h"
#include <algorithm>
#include <cstdlib>
#include "cache.h"

void dcpt_opt::age_lru()
{
    for (auto& e : table)
        if (e.valid) e.lru_age++;
}

int dcpt_opt::find_entry(uint64_t pc)
{
    for (int i = 0; i < NUM_ENTRIES; i++)
        if (table[i].valid && table[i].pc_tag == pc)
            return i;
    return -1;
}

int dcpt_opt::allocate_entry(uint64_t pc, uint64_t block)
{
    for (int i = 0; i < NUM_ENTRIES; i++) {
        if (!table[i].valid) {
            table[i]            = entry{};
            table[i].pc_tag     = pc;
            table[i].last_block = block;
            table[i].valid      = true;
            return i;
        }
    }

    // All slots occupied — evict the least recently used entry.
    int lru = 0;
    for (int i = 1; i < NUM_ENTRIES; i++)
        if (table[i].lru_age > table[lru].lru_age) lru = i;

    table[lru]            = entry{};
    table[lru].pc_tag     = pc;
    table[lru].last_block = block;
    table[lru].valid      = true;
    return lru;
}

int dcpt_opt::degree_for_conf(int conf, double mshr) const
{
    int base = 0;
    if      (conf >= CONF_FULL) base = MAX_DEGREE;
    else if (conf >= CONF_HIT)  base = 2;
    else if (conf >= CONF_PREF) base = 1;

    // Back off under MSHR pressure to avoid stalling demand traffic.
    if (mshr > 0.75) return 1;
    if (mshr > 0.50) return std::min(base, 2);
    return base;
}

void dcpt_opt::do_prefetch_stride(entry& e, uint64_t block,
                                  champsim::address addr,
                                  uint32_t metadata_in, int degree)
{
    for (int i = 1; i <= degree; i++) {
        int64_t target = block + i * e.stride;
        if (target < 0) break;

        champsim::address pf_addr{
            champsim::block_number{static_cast<uint64_t>(target)}};

        prefetch_line(pf_addr, true, metadata_in);
    }
}

void dcpt_opt::do_prefetch_dcpt(entry& e, int match_idx, uint64_t block,
                                champsim::address addr,
                                uint32_t metadata_in, int degree)
{
    // Replay the sequence of deltas that historically followed match_idx.
    // Each delta is added cumulatively so we build up the full prefetch offset.
    int64_t offset = 0;

    for (int i = 1; i <= degree; i++) {
        int     idx   = (match_idx + i) % HISTORY_LEN;
        int64_t delta = e.deltas[idx];
        if (delta == 0) break;

        offset += delta;
        int64_t target = block + offset;
        if (target < 0) break;

        champsim::address pf_addr{
            champsim::block_number{static_cast<uint64_t>(target)}};

        // Let the first prefetch cross a page boundary — large stride patterns
        // need it.  Stop subsequent ones to avoid runaway speculation.
        if (i > 1 && !intern_->virtual_prefetch &&
            champsim::page_number{pf_addr} != champsim::page_number{addr})
            break;

        prefetch_line(pf_addr, true, metadata_in);
    }
}

uint32_t dcpt_opt::prefetcher_cache_operate(
    champsim::address addr, champsim::address ip,
    uint8_t cache_hit, bool useful_prefetch,
    access_type type, uint32_t metadata_in)
{
    if (type == access_type::PREFETCH) return metadata_in;

    using namespace champsim::data::data_literals;

    uint64_t block = champsim::block_number{addr}.to<uint64_t>();
    uint64_t pc    = ip.slice_upper<2_b>().to<uint64_t>();

    age_lru();

    int idx = find_entry(pc);
    if (idx == -1) {
        allocate_entry(pc, block);
        return metadata_in;
    }

    auto& e   = table[idx];
    e.lru_age = 0;

    int64_t delta  = static_cast<int64_t>(block) - static_cast<int64_t>(e.last_block);
    e.last_block   = block;

    if (delta == 0) return metadata_in;

    // Skip large jumps — they're usually context switches or irregular pointer
    // dereferences that don't carry useful stride information.
    if (std::abs(delta) > 64) return metadata_in;

    int64_t prev_delta = e.last_delta;

    if (delta == prev_delta)
        e.stride_conf++;
    else
        e.stride_conf = 0;

    if (e.stride_conf >= 2)
        e.stride = delta;

    e.last_delta = delta;

    double mshr   = intern_->get_mshr_occupancy_ratio();
    int    degree = degree_for_conf(e.conf, mshr);

    // If the stride has been stable for at least two consecutive accesses,
    // use the simpler stride path rather than the full delta-correlation search.
    if (e.stride_conf >= 2 && degree > 0) {
        do_prefetch_stride(e, block, addr, metadata_in, degree);
        return metadata_in;
    }

    // DCPT match: search history for the most recent occurrence of the
    // two-delta pair (prev_delta, delta).  Matching a pair rather than a
    // single delta reduces false positives on irregular access streams.
    int match_idx = -1;
    for (int i = 2; i <= HISTORY_LEN; i++) {
        int idx1 = (e.head - i + HISTORY_LEN) % HISTORY_LEN;
        int idx2 = (idx1  - 1 + HISTORY_LEN) % HISTORY_LEN;

        if (e.deltas[idx1] == delta && e.deltas[idx2] == prev_delta) {
            match_idx = idx1;
            break;
        }
    }

    e.deltas[e.head] = delta;
    e.head = (e.head + 1) % HISTORY_LEN;

    if (match_idx != -1)
        e.conf = std::min(e.conf + 1, CONF_MAX);
    else
        e.conf = std::max(e.conf - 1, 0);

    if (useful_prefetch)
        e.conf = std::min(e.conf + 1, CONF_MAX);

    if (degree == 0 || match_idx == -1)
        return metadata_in;

    // On streaming workloads the prefetcher is often working when the CPU
    // hits in L2, so we also issue on hits once confidence is high enough.
    bool issue = !cache_hit || (e.conf >= CONF_HIT);
    if (!issue) return metadata_in;

    do_prefetch_dcpt(e, match_idx, block, addr, metadata_in, degree);
    return metadata_in;
}
