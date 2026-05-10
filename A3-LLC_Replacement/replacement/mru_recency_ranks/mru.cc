#include "mru.h"

#include <algorithm>

mru::mru(CACHE* cache) : mru(cache, cache->NUM_SET, cache->NUM_WAY) {}

mru::mru(CACHE* cache, long sets, long ways)
    : replacement(cache),
      NUM_WAY(ways),
      // Initialize all ranks to 0. Invalid ways will be preferred by find_victim
      // before ranks are ever consulted, so the initial value is harmless.
      rank(static_cast<std::size_t>(sets * ways), 0) {}

// ============================================================
//  touch(): Promote the accessed way to rank 0 (MRU position).
//  All other valid ways in the set have their rank incremented by 1,
//  clamped to NUM_WAY - 1. This maintains a strict total order of
//  recency within each set without needing a global cycle counter.
// ============================================================
void mru::touch(long set, long way)
{
    std::size_t base = static_cast<std::size_t>(set) * NUM_WAY;
    uint8_t max_rank = static_cast<uint8_t>(NUM_WAY - 1);

    // Age all other ways in this set
    for (long w = 0; w < NUM_WAY; ++w) {
        if (w != way && rank[base + w] < max_rank)
            rank[base + w]++;
    }

    // Promote accessed way to MRU (rank 0)
    rank[base + way] = 0;
}

// ============================================================
//  find_victim(): MRU eviction.
//  1. Prefer any invalid way (lowest index first).
//  2. Among valid ways, evict the one with rank 0 (most recently used).
//     Ties at rank 0 are broken by lowest way index (natural scan order).
// ============================================================
long mru::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set,
                      const champsim::cache_block* current_set, champsim::address ip,
                      champsim::address full_addr, access_type type)
{
    // Always prefer an invalid way
    for (long way = 0; way < NUM_WAY; way++) {
        if (!current_set[way].valid)
            return way;
    }

    // All ways valid — find lowest-index way with rank 0
    std::size_t base = static_cast<std::size_t>(set) * NUM_WAY;
    for (long w = 0; w < NUM_WAY; ++w) {
        if (rank[base + w] == 0)
            return w;
    }

    // Fallback (should never happen if ranks are maintained correctly):
    // evict way 0
    return 0;
}

// ============================================================
//  replacement_cache_fill(): Every fill stamps the way as MRU.
// ============================================================
void mru::replacement_cache_fill(uint32_t triggering_cpu, long set, long way,
                                 champsim::address full_addr, champsim::address ip,
                                 champsim::address victim_addr, access_type type)
{
    touch(set, way);
}

// ============================================================
//  update_replacement_state(): On demand hits, promote to MRU.
//  Writeback hits are skipped (same convention as baseline LRU).
// ============================================================
void mru::update_replacement_state(uint32_t triggering_cpu, long set, long way,
                                   champsim::address full_addr, champsim::address ip,
                                   champsim::address victim_addr, access_type type,
                                   uint8_t hit)
{
    if (!hit) return;
    if (access_type{type} == access_type::WRITE) return; // Skip writeback hits

    touch(set, way);
}