#include "hash_bop.h"
#include "cache.h"

uint32_t hash_bop::prefetcher_cache_operate(
    champsim::address addr, champsim::address ip,
    uint8_t cache_hit, bool useful_prefetch,
    access_type type, uint32_t metadata_in)
{
    // only train on demand loads
    if (type != access_type::LOAD)
        return metadata_in;

    uint64_t curr_blk = champsim::block_number{addr}.to<uint64_t>();

    // check all candidate offsets in the hash table
    for (int i = 0; i < 20; i++) {
        uint64_t past_blk = curr_blk - offsets[i];
        uint32_t idx = hash_addr(past_blk);

        // if we have a match, increment the score for this offset
        if (history_table[idx] == past_blk) {
            scores[i]++;
        }
    }

    // insert current access into the table (overwrites on collision)
    history_table[hash_addr(curr_blk)] = curr_blk;

    // end of phase evaluation
    timer++;
    if (timer >= PHASE_MAX) {
        int max_score = 0;
        int winner_idx = 0;

        for (int i = 0; i < 20; i++) {
            if (scores[i] > max_score) {
                max_score = scores[i];
                winner_idx = i;
            }
            scores[i] = 0; // clear for next round
        }

        // check if the winning offset meets our threshold
        if (max_score >= THRESHOLD) {
            best_offset = offsets[winner_idx];
            pref_degree = 3;
        } else {
            // fallback to next-line if no pattern is found
            best_offset = 1;
            pref_degree = 1;
        }

        timer = 0; // reset phase
    }

    // issue the prefetches
    for (int d = 1; d <= pref_degree; d++) {
        uint64_t target = curr_blk + (best_offset * d);
        champsim::address pf_addr{target << LOG2_BLOCK_SIZE};

        // don't cross 4KB page boundaries
        if (champsim::page_number{addr} != champsim::page_number{pf_addr})
            break;

        // check MSHR pressure before issuing
        double mshr_load = intern_->get_mshr_occupancy_ratio();
        bool fill_l2 = (mshr_load < 0.80);

        prefetch_line(pf_addr, fill_l2, metadata_in);
    }

    return metadata_in;
}