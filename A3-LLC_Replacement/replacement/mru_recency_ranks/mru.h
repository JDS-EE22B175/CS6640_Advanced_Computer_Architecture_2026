#ifndef REPLACEMENT_MRU_H
#define REPLACEMENT_MRU_H

#include <cstdint>
#include <vector>

#include "cache.h"
#include "modules.h"

// ============================================================
//  MRU (Most Recently Used) Replacement — Recency Rank Version
//
//  Each way stores a uint8_t recency rank in [0, NUM_WAY-1]:
//    rank 0  = most recently used  (MRU victim)
//    rank 15 = least recently used
//
//  On every hit or fill:
//    1. Set accessed way's rank to 0.
//    2. Increment every other way's rank by 1, clamped to NUM_WAY-1.
//
//  MRU victim = lowest-index way with rank 0.
//
//  Storage: 2048 × 16 × 1 byte = 32 KB  (vs 256 KB for uint64_t timestamps)
// ============================================================

class mru : public champsim::modules::replacement
{
  long NUM_WAY;

  // Per-way recency ranks: rank[set * NUM_WAY + way] ∈ [0, NUM_WAY-1]
  // rank 0 = most recently used, rank NUM_WAY-1 = least recently used
  std::vector<uint8_t> rank;

  // Helper: promote a way to rank 0 and age all other ways in the set.
  void touch(long set, long way);

public:
  explicit mru(CACHE* cache);
  mru(CACHE* cache, long sets, long ways);

  long find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set, const champsim::cache_block* current_set, champsim::address ip,
                   champsim::address full_addr, access_type type);
  void replacement_cache_fill(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip, champsim::address victim_addr,
                              access_type type);
  void update_replacement_state(uint32_t triggering_cpu, long set, long way, champsim::address full_addr, champsim::address ip, champsim::address victim_addr,
                                access_type type, uint8_t hit);
};

#endif