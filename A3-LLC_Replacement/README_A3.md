# CS6640 Advanced Computer Architecture - A3 LLC Replacement

This repository contains various Last-Level Cache (LLC) replacement policies implemented in ChampSim. The goal of this assignment was to maximize IPC across four diverse workload traces (T1-T4) within a strict 128KB metadata budget.

## Final Performance Highlights
*   **Best Policy:** Tri-Mode Tournament Adaptive (`adaptive_final_tournament`)
*   **Peak GeoMean IPC:** 0.6565
*   **Key Optimizations:** 
    *   Set Dueling to avoid Miss-IPC paradoxes
    *   Writeback bypassing to prevent cache pollution from L2 dirty evictions
    *   Metadata bit-packing to strictly adhere to the 128KB budget

---

## Repository Structure & Policy Evolution

This repository includes several versions of our policies to showcase the architectural evolution and hardware-cost analysis.

### 1. The Adaptive Evolution
*   **`adaptive_v1_drrip/`**
    *   **Description:** Dynamic Re-Reference Interval Prediction. Dueling SRRIP vs BRRIP to handle thrashing workloads.
*   **`adaptive_v2_srrip_mru_duel/`**
    *   **Description:** Early Set Dueling attempt pitting SRRIP against MRU.
*   **`adaptive_v3_pure_ship/`**
    *   **Description:** Pure SHiP (Signature History-based Insertion Policy) with a custom recovery throttle. Set Dueling was temporarily abandoned here after discovering the "Miss-IPC Paradox" on Trace 4, where higher misses actually yielded higher IPC due to Memory-Level Parallelism.
*   **`adaptive_v4_lru_srrip_duel/`**
    *   **Description:** Set Dueling between LRU and SRRIP with expanded sampling (64 leader sets) to eliminate noise. Pits True Recency against Scan Resistance.
*   **`adaptive_final_tournament/` (Highest IPC: 0.6565)**
    *   **Description:** Tri-Mode Tournament dueling LRU vs SRRIP vs SHiP. By increasing the sample sets to 64, we eliminated sampling noise and achieved peak performance.
*   **`mpp_final/` (IPC: 0.6541)**
    *   **Description:** A lightweight Neural Network (Multiperspective Perceptron) predictor. It hashes 5 distinct features (PC, Page, History, etc.) to predict dead blocks.

### 2. MRU Hardware-Cost Analysis
We implemented two variants of MRU to analyze the tradeoff between metadata cost and temporal precision. Interestingly, both achieved nearly identical performance (~0.5294 IPC).
*   **`mru_recency_ranks/`**
    *   **Description:** A hardware-efficient "Pseudo-MRU". Uses a 4-bit saturating array (0-15) for recency ranking, requiring only 4 bits per cache line. Ties are broken by way-index.
*   **`mru_exact/`**
    *   **Description:** A hardware-expensive "True-MRU". Uses the native ChampSim 64-bit absolute `cycle` timestamp, requiring 64 bits per cache line. 

### 3. The SRRIP Baseline
*   **`srrip/`**
    *   **Description:** The polished SRRIP policy. It contains a critical "writeback bypass" fix: L2 dirty evictions (`access_type::WRITE`) are inserted at `maxRRPV` to prevent them from polluting the cache, as they do not indicate immediate demand reuse.

---

## How to Run These Policies

ChampSim compilation requires the C++ file name and the internal class name to match the configuration folder name. 

To run any of the historical or alternate variants in this repository:
1. Create a folder inside ChampSim's `replacement/` directory named exactly what you want the policy to be called in your config (e.g., `replacement/adaptive/`).
2. Copy the `.cc` file from one of our versioned folders (e.g., `adaptive_v3_pure_ship/adaptive.cc`) into your new folder.
3. Update your `champsim_config.json` to use `"replacement": "adaptive"`.
4. Compile and run:
    ```bash
    ./config.sh champsim_config.json
    make
    ```

## Results & Logs
Raw simulation logs (`.txt`) for traces T1-T4 are included inside the respective policy folders or the `results/` directory.

We have also included our automated data pipeline scripts:
*   `compare_all_results.py`: Generates the master global leaderboard.
*   `compare_by_policy.py`: Generates isolated CSV leaderboards for individual policy architectures.
