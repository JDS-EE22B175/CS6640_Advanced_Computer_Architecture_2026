// mpp.cc — Multiperspective Perceptron (MPP) LLC Replacement
//
// A lightweight neural network predicts reuse vs dead on every cache fill.
// 5 feature tables capture orthogonal dimensions of cache behavior:
//   0: PC alone         — basic instruction-level behavior
//   1: Memory Page      — spatial data structure locality
//   2: PC XOR Page      — instruction-data correlation
//   3: PC XOR Type      — read/write/prefetch behavior per instruction
//   4: PC XOR History   — temporal phase detection
//
// Training is performed exclusively on 128 Sampler Sets that store full
// metadata. The remaining 1920 follower sets only store 2-bit RRPV.

#include "mpp.h"
#include <algorithm>
#include <iostream>
#include <numeric>

mpp::mpp(CACHE* cache)
    : champsim::modules::replacement(cache),
      my_cache(cache),
      global_history(0),
      total_evictions(0),
      predicted_reuse(0),
      predicted_dead(0),
      train_positive(0),
      train_negative(0),
      initialized(false) {}

void mpp::check_init() {
    if (initialized) return;
    initialized = true;

    long num_sets = my_cache->NUM_SET;
    long num_ways = my_cache->NUM_WAY;
    long total_lines = num_sets * num_ways;

    // 2-bit packed RRPV: 4 lines per byte
    rrpv_packed.assign((total_lines + 3) / 4, 0xFF);  // All RRPV=3

    // 5 weight tables, 16K entries each, initialized to 0
    weights.assign(NUM_TABLES * TABLE_SIZE, 0);

    // Sampler: 128 sets × 16 ways
    sampler.assign(SAMPLER_SETS * SAMPLER_WAYS, SamplerEntry{0, 0, 0, 0, 0, false, false});

    // Select sampler sets uniformly across the cache
    sampler_set_map.resize(SAMPLER_SETS);
    uint32_t stride = static_cast<uint32_t>(num_sets / SAMPLER_SETS);
    for (uint32_t i = 0; i < SAMPLER_SETS; ++i) {
        sampler_set_map[i] = i * stride;
    }
}

// ============================================================
//  Hash functions — 5 orthogonal perspectives
// ============================================================

uint32_t mpp::hash_pc(champsim::address ip) const {
    uint64_t pc = ip.to<uint64_t>();
    return static_cast<uint32_t>((pc >> 2) ^ (pc >> 16)) & TABLE_MASK;
}

uint32_t mpp::hash_page(champsim::address addr) const {
    uint64_t a = addr.to<uint64_t>() >> 12;  // page number
    return static_cast<uint32_t>((a ^ (a >> 8) ^ (a >> 16))) & TABLE_MASK;
}

uint32_t mpp::hash_pc_page(champsim::address ip, champsim::address addr) const {
    uint64_t pc = ip.to<uint64_t>() >> 2;
    uint64_t page = addr.to<uint64_t>() >> 12;
    return static_cast<uint32_t>((pc ^ page ^ (pc >> 7) ^ (page >> 5))) & TABLE_MASK;
}

uint32_t mpp::hash_pc_type(champsim::address ip, access_type type) const {
    uint64_t pc = ip.to<uint64_t>() >> 2;
    uint64_t t = static_cast<uint64_t>(type);
    return static_cast<uint32_t>((pc ^ (t << 11) ^ (pc >> 5))) & TABLE_MASK;
}

uint32_t mpp::hash_pc_hist(champsim::address ip) const {
    uint64_t pc = ip.to<uint64_t>() >> 2;
    return static_cast<uint32_t>((pc ^ global_history ^ (pc >> 9))) & TABLE_MASK;
}

// ============================================================
//  Perceptron predict: sum weights from all 5 perspectives
// ============================================================

int mpp::predict(champsim::address ip, champsim::address addr, access_type type) const {
    int sum = 0;
    sum += weight(0, hash_pc(ip));
    sum += weight(1, hash_page(addr));
    sum += weight(2, hash_pc_page(ip, addr));
    sum += weight(3, hash_pc_type(ip, type));
    sum += weight(4, hash_pc_hist(ip));
    return sum;
}

// ============================================================
//  Perceptron train: reinforce or punish all 5 perspectives
// ============================================================

void mpp::train(champsim::address ip, champsim::address addr, access_type type,
                uint16_t pc_sig, uint16_t page_sig, uint8_t type_sig, uint8_t hist_sig,
                bool reused) {
    // Use stored signatures for training (not current state)
    int8_t delta = reused ? 1 : -1;

    auto clamp_update = [&](uint32_t table, uint32_t idx) {
        int8_t& w = weight(table, idx);
        int new_val = static_cast<int>(w) + delta;
        if (new_val > WEIGHT_MAX) new_val = WEIGHT_MAX;
        if (new_val < WEIGHT_MIN) new_val = WEIGHT_MIN;
        w = static_cast<int8_t>(new_val);
    };

    clamp_update(0, pc_sig);
    clamp_update(1, page_sig);
    clamp_update(2, pc_sig ^ page_sig);
    clamp_update(3, pc_sig ^ type_sig);
    clamp_update(4, pc_sig ^ hist_sig);
}

// ============================================================
//  Sampler set detection
// ============================================================

bool mpp::is_sampler_set(long set) const {
    uint32_t stride = my_cache->NUM_SET / SAMPLER_SETS;
    return (set % stride == 0) && (set / stride < SAMPLER_SETS);
}

uint32_t mpp::get_sampler_index(long set) const {
    uint32_t stride = static_cast<uint32_t>(my_cache->NUM_SET / SAMPLER_SETS);
    return static_cast<uint32_t>(set / stride);
}

// ============================================================
//  RRIP victim selection
// ============================================================

long mpp::find_victim_rrip(long set) {
    long offset = set * my_cache->NUM_WAY;
    while (true) {
        for (long w = 0; w < my_cache->NUM_WAY; ++w) {
            if (get_rrpv(offset + w) == MAX_RRPV) return w;
        }
        for (long w = 0; w < my_cache->NUM_WAY; ++w) {
            uint8_t r = get_rrpv(offset + w);
            if (r < MAX_RRPV) set_rrpv(offset + w, r + 1);
        }
    }
}

// ============================================================
//  find_victim: prefer invalid, then RRIP; train sampler on eviction
// ============================================================

long mpp::find_victim(uint32_t triggering_cpu, uint64_t instr_id, long set,
                       const champsim::cache_block* current_set,
                       champsim::address ip, champsim::address full_addr,
                       access_type type) {
    check_init();

    for (long w = 0; w < my_cache->NUM_WAY; ++w) {
        if (!current_set[w].valid) return w;
    }

    total_evictions++;
    long victim = find_victim_rrip(set);

    // Train on sampler eviction (negative reinforcement for dead blocks)
    if (is_sampler_set(set)) {
        uint32_t si = get_sampler_index(set);
        size_t s_idx = si * SAMPLER_WAYS + victim;
        if (sampler[s_idx].valid && !sampler[s_idx].used) {
            SamplerEntry& e = sampler[s_idx];
            // This line was never reused — train negatively
            // Reconstruct addresses from signatures for training
            train(ip, full_addr, type, e.pc_sig, e.page_sig, e.type_sig, e.hist_sig, false);
            train_negative++;
        }
    }

    return victim;
}

// ============================================================
//  replacement_cache_fill: predict reuse, set RRPV, populate sampler
// ============================================================

void mpp::replacement_cache_fill(uint32_t triggering_cpu, long set, long way,
                                  champsim::address full_addr, champsim::address ip,
                                  champsim::address victim_addr, access_type type) {
    check_init();
    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY) return;

    size_t idx = set * my_cache->NUM_WAY + way;

    // Perceptron prediction
    int prediction = predict(ip, full_addr, type);

    if (prediction >= 0) {
        set_rrpv(idx, 0);             // predicted reuse → near-MRU
        predicted_reuse++;
    } else {
        set_rrpv(idx, MAX_RRPV - 1);  // predicted dead → distant
        predicted_dead++;
    }

    // Populate sampler if this is a sampler set
    if (is_sampler_set(set)) {
        uint32_t si = get_sampler_index(set);
        size_t s_idx = si * SAMPLER_WAYS + way;
        sampler[s_idx].tag = static_cast<uint32_t>((full_addr.to<uint64_t>() >> 6) & 0xFFFFFFFF);
        sampler[s_idx].pc_sig = static_cast<uint16_t>(hash_pc(ip));
        sampler[s_idx].page_sig = static_cast<uint16_t>(hash_page(full_addr));
        sampler[s_idx].type_sig = static_cast<uint8_t>(type);
        sampler[s_idx].hist_sig = static_cast<uint8_t>(global_history & 0xFF);
        sampler[s_idx].valid = true;
        sampler[s_idx].used = false;
    }

    // Update global history
    uint64_t pc = ip.to<uint64_t>() >> 2;
    global_history = ((global_history << 1) ^ (pc & 0x3FFF)) & 0xFFFFFFFF;
}

// ============================================================
//  update_replacement_state: train on sampler hits
// ============================================================

void mpp::update_replacement_state(uint32_t triggering_cpu, long set, long way,
                                    champsim::address full_addr, champsim::address ip,
                                    champsim::address victim_addr, access_type type,
                                    uint8_t hit) {
    check_init();
    if (set >= my_cache->NUM_SET || way >= my_cache->NUM_WAY) return;
    if (!hit) return;

    // Skip writeback hits (consistent with LRU and SRRIP baselines)
    if (access_type{type} == access_type::WRITE) return;

    size_t idx = set * my_cache->NUM_WAY + way;
    set_rrpv(idx, 0);  // Hit → promote to MRU

    // Train perceptron on sampler hit (positive reinforcement)
    if (is_sampler_set(set)) {
        uint32_t si = get_sampler_index(set);
        size_t s_idx = si * SAMPLER_WAYS + way;
        if (sampler[s_idx].valid && !sampler[s_idx].used) {
            sampler[s_idx].used = true;
            SamplerEntry& e = sampler[s_idx];
            train(ip, full_addr, type, e.pc_sig, e.page_sig, e.type_sig, e.hist_sig, true);
            train_positive++;
        }
    }

    // Update global history on hits too
    uint64_t pc = ip.to<uint64_t>() >> 2;
    global_history = ((global_history << 1) ^ (pc & 0x3FFF)) & 0xFFFFFFFF;
}

void mpp::replacement_final_stats() {
    std::string name = my_cache->NAME;
    if (name.find("LLC") == std::string::npos) return;

    std::cout << "MPP LLC predicted_reuse: " << predicted_reuse
              << " predicted_dead: " << predicted_dead << std::endl;
    std::cout << "MPP LLC train_positive: " << train_positive
              << " train_negative: " << train_negative << std::endl;
    std::cout << "MPP LLC total_evictions: " << total_evictions << std::endl;
}
