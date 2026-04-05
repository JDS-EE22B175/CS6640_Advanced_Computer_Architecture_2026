#include "bop_ultimate.h"
#include "cache.h"

uint32_t bop_ultimate::prefetcher_cache_operate(
    champsim::address addr, champsim::address ip,
    uint8_t cache_hit, bool useful_prefetch,
    access_type type, uint32_t metadata_in)
{
    if (type != access_type::LOAD)
        return metadata_in;

    uint64_t base_blk = champsim::block_number{addr}.to<uint64_t>();

    // 1. Evaluate Candidate Offsets
    for (std::size_t i = 0; i < test_offsets.size(); ++i) {
        uint64_t expected_past_blk = base_blk - test_offsets[i];
        
        // Scan the RR buffer for the theoretical source block
        for (std::size_t j = 0; j < RR_CAPACITY; ++j) {
            if (history_ring[j] == expected_past_blk) {
                offset_scores[i]++;
                break;
            }
        }
    }

    // 2. Update the Recent Request (RR) Buffer
    history_ring[ring_ptr] = base_blk;
    ring_ptr = (ring_ptr + 1) % RR_CAPACITY;

    // 3. Phase Evaluation & Stride Selection
    phase_counter++;
    if (phase_counter >= PHASE_LENGTH) {
        int         max_score = 0;
        std::size_t best_idx  = 0;

        for (std::size_t i = 0; i < test_offsets.size(); ++i) {
            if (offset_scores[i] > max_score) {
                max_score = offset_scores[i];
                best_idx  = i;
            }
            offset_scores[i] = 0; // Reset for the next learning phase
        }

        // Lock in the new global stride if it meets the confidence bar
        if (max_score > MIN_HITS) {
            active_offset = test_offsets[best_idx];
            issue_depth   = 3;
        } else {
            // Fallback mechanism: conservative next-line scraping
            active_offset = 1;
            issue_depth   = 1;
        }

        phase_counter = 0;
    }

    // 4. Prefetch Generation & Issuance
    for (int k = 1; k <= issue_depth; k++) {
        uint64_t projected_blk = base_blk + (active_offset * k);
        champsim::address target_addr{projected_blk << LOG2_BLOCK_SIZE};

        // Enforce OS page boundaries
        if (champsim::page_number{addr} != champsim::page_number{target_addr})
            break;

        // Dynamic fill-level control based on MSHR pressure
        bool bypass_l2 = intern_->get_mshr_occupancy_ratio() < 0.80;
        prefetch_line(target_addr, bypass_l2, metadata_in);
    }

    return metadata_in;
}