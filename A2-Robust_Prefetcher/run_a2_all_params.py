import json
import os
import subprocess
import copy
import sys
import threading
import time
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor, as_completed

# ==========================================
# Configuration Paths
# ==========================================
BASE_CONFIG_PATH = 'base_config.json'
CONFIG_OUT_DIR   = 'a2_generated_configs'
RESULTS_DIR      = 'a2_results'
TRACES_DIR       = 'traces/A2/'

WARMUP = 20000000
SIM    = 50000000

TRACES = [
    "470.lbm-1274B.champsimtrace.xz",
    "603.bwaves_s-1740B.champsimtrace.xz",
    "429.mcf-217B.champsimtrace.xz",
    "605.mcf_s-1152B.champsimtrace.xz",
    "473.astar-359B.champsimtrace.xz",
    "620.omnetpp_s-874B.champsimtrace.xz"
]

PREFETCHERS = ["ip_stride", "ghb_stride"]
# PREFETCHERS = ["bop"]

# ==========================================
# Base config values (for reference — these are NOT re-run in sweeps)
# ==========================================
# ooo_cpu:  rob_size=192, lq_size=72, *_width=4
# L1D:      mshr_size=16
# L2C:      sets=512, ways=8, mshr_size=16, replacement=lru
# LLC:      sets=2048, ways=16, replacement=ship
# physical: channels=1, tCAS=16, tRCD=16, tRP=16, tRAS=52
# top-level: block_size=64, page_size=8192

# ==========================================
# Experiments
# ==========================================

BASELINE_EXPERIMENTS = {
    "baseline": { ("L2C", "latency"): [12] },
}

PREFETCHER_EXPERIMENTS = {
    "lq_size": {
        ("ooo_cpu", 0, "lq_size"): [32, 128],
    },
    "issue_width": {
        ("ooo_cpu", 0, "fetch_width"):    [2, 6, 8],
        ("ooo_cpu", 0, "decode_width"):   [2, 6, 8],
        ("ooo_cpu", 0, "dispatch_width"): [2, 6, 8],
        ("ooo_cpu", 0, "execute_width"):  [2, 6, 8],
        ("ooo_cpu", 0, "retire_width"):   [2, 6, 8],
    },
    "rob_size": {
        ("ooo_cpu", 0, "rob_size"): [96, 256, 384],
    },
    "l1d_mshr": {
        ("L1D", "mshr_size"): [8, 32],
    },
    "l2c_mshr": {
        ("L2C", "mshr_size"): [8, 32, 64],
    },
    "llc_size": {
        ("LLC", "sets"): [1024, 4096],
    },
    "l2c_assoc": {
        ("L2C", "sets"): [1024, 256],
        ("L2C", "ways"): [   4,  16],
    },
    "block_sz": {
        ("block_size",): [32, 128],
    },
    "llc_repl": {
        ("LLC", "replacement"): ["lru", "srrip"],
    },
    "dram_ch": {
        ("physical_memory", "channels"): [2, 4],
    },
    "dram_lat": {
        ("physical_memory", "tCAS"): [ 8, 24],
        ("physical_memory", "tRCD"): [ 8, 24],
        ("physical_memory", "tRP"):  [ 8, 24],
        ("physical_memory", "tRAS"): [26, 78],
    },
    "page_sz": {
        ("page_size",): [4096, 65536, 262144],
    },
}

LOCKSTEP_EXPERIMENTS = {"issue_width", "l2c_assoc", "dram_lat"}

# ==========================================
# Tuning knobs
# ==========================================
MAX_SIM_WORKERS = 8
MAKE_JOBS       = 4
WRITE_BUFFER    = 256 * 1024

_compile_lock = threading.Lock()


# ==========================================
# Helpers
# ==========================================

def fmt_time(seconds):
    seconds = int(seconds)
    h, rem = divmod(seconds, 3600)
    m, s   = divmod(rem, 60)
    if h:
        return f"{h}h {m:02d}m {s:02d}s"
    return f"{m}m {s:02d}s"


def setup_directories():
    for d in (CONFIG_OUT_DIR, RESULTS_DIR):
        os.makedirs(d, exist_ok=True)


def load_base_config():
    with open(BASE_CONFIG_PATH, 'r') as f:
        return json.load(f)


def set_nested(config, key_path, value):
    node = config
    for key in key_path[:-1]:
        node = node[key]
    node[key_path[-1]] = value


def apply_experiment(config, param_dict, sweep_point):
    if len(param_dict) == 1:
        key_path, _ = next(iter(param_dict.items()))
        set_nested(config, key_path, sweep_point)
    else:
        for key_path, values in param_dict.items():
            set_nested(config, key_path, values[sweep_point])


def sweep_points(exp_name, param_dict):
    if exp_name in LOCKSTEP_EXPERIMENTS:
        n = len(next(iter(param_dict.values())))
        return list(range(n))
    else:
        _, values = next(iter(param_dict.items()))
        return values


def build_config_list(base_config):
    configs = []
    for pref in PREFETCHERS:
        for exp_name, param_dict in BASELINE_EXPERIMENTS.items():
            for sp in sweep_points(exp_name, param_dict):
                cfg = copy.deepcopy(base_config)
                cfg["L2C"]["prefetcher"] = pref
                apply_experiment(cfg, param_dict, sp)
                fname = f"{pref}_{exp_name}_{sp}.json"
                configs.append((cfg, fname, pref, exp_name, sp))

    for pref in PREFETCHERS:
        working_base = copy.deepcopy(base_config)
        working_base["L2C"]["prefetcher"] = pref

        for exp_name, param_dict in PREFETCHER_EXPERIMENTS.items():
            for sp in sweep_points(exp_name, param_dict):
                cfg = copy.deepcopy(working_base)
                apply_experiment(cfg, param_dict, sp)
                fname = f"{pref}_{exp_name}_{sp}.json"
                configs.append((cfg, fname, pref, exp_name, sp))

    return configs


def write_lockstep_sidecars():
    os.makedirs(RESULTS_DIR, exist_ok=True)
    for exp_name in LOCKSTEP_EXPERIMENTS:
        param_dict = PREFETCHER_EXPERIMENTS[exp_name]
        n          = len(next(iter(param_dict.values())))
        mapping    = {
            str(i): {str(kp[-1]): v[i] for kp, v in param_dict.items()}
            for i in range(n)
        }
        path = os.path.join(RESULTS_DIR, f"{exp_name}_index_map.json")
        with open(path, 'w') as f:
            json.dump(mapping, f, indent=2)


def run_command(cmd, log_file=None):
    if log_file:
        with open(log_file, 'w', buffering=WRITE_BUFFER) as f:
            subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, check=True)
    else:
        subprocess.run(cmd, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ==========================================
# Compile
# ==========================================

def compile_for_config(config, config_filename):
    config_filepath = os.path.join(CONFIG_OUT_DIR, config_filename)
    with open(config_filepath, 'w') as f:
        json.dump(config, f, indent=2)

    subprocess.run(["./config.sh", config_filepath],
                   check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                   
    # Ensure clean environment to prevent exit status 2 linker errors
    subprocess.run(["make", "clean"], check=True, 
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                   
    subprocess.run(["make", f"-j{MAKE_JOBS}"],
                   check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    bin_dir = "bin"
    files   = [os.path.join(bin_dir, fn)
               for fn in os.listdir(bin_dir) if not fn.endswith('.xz')]
    latest  = max(files, key=os.path.getmtime)

    unique_binary = os.path.join(
        bin_dir, f"champsim_{config_filename.replace('.json', '')}")
    subprocess.run(["cp", latest, unique_binary], check=True)
    return unique_binary


def compile_and_simulate(config, config_filename, pref, exp_name, val,
                         sim_executor):
                         
    # Calculate expected binary name to check if compilation is needed
    expected_binary = os.path.join("bin", f"champsim_{config_filename.replace('.json', '')}")
    
    if os.path.exists(expected_binary):
        print(f"[COMPILE] SKIP -> {expected_binary} already exists.")
        binary_path = expected_binary
    else:
        with _compile_lock:
            print(f"[COMPILE] {config_filename}")
            t0 = time.perf_counter()
            try:
                binary_path = compile_for_config(config, config_filename)
            except subprocess.CalledProcessError as e:
                print(f"[ERROR] Compile {pref}/{exp_name}={val}: {e}")
                return []
            print(f"[COMPILE] Done -> {binary_path}  "
                  f"({fmt_time(time.perf_counter() - t0)})")

    return [
        sim_executor.submit(run_simulation,
                            binary_path, pref, exp_name, val, trace)
        for trace in TRACES
    ]


# ==========================================
# Simulate
# ==========================================

def run_simulation(binary_path, pref, exp_name, val, trace):
    trace_path      = os.path.join(TRACES_DIR, trace)
    trace_shortname = trace.split('-')[0]
    result_log      = os.path.join(
        RESULTS_DIR, f"{pref}_{exp_name}_{val}_{trace_shortname}.txt")

    if os.path.exists(result_log):
        with open(result_log, 'r') as f:
            if "Region of Interest Statistics" in f.read():
                return f"[SKIP] {result_log}"

    cmd = [
        binary_path,
        "--warmup_instructions", str(WARMUP),
        "--simulation_instructions", str(SIM),
        trace_path
    ]

    t0 = time.perf_counter()
    run_command(cmd, log_file=result_log)
    return (f"[DONE] {pref}/{exp_name}={val}/{trace_shortname}"
            f"  ({fmt_time(time.perf_counter() - t0)})")


# ==========================================
# Main
# ==========================================

def main():
    wall_start = time.perf_counter()

    for trace in TRACES:
        p = os.path.join(TRACES_DIR, trace)
        if not os.path.exists(p):
            print(f"[FATAL] Trace missing: {p}")
            sys.exit(1)

    setup_directories()
    write_lockstep_sidecars()

    base_config = load_base_config()
    all_configs = build_config_list(base_config)

    total_configs = len(all_configs)
    total_sims    = total_configs * len(TRACES)
    print(f"Configs to process : {total_configs}")
    print(f"Simulation jobs    : {total_sims}  ({MAX_SIM_WORKERS} parallel workers)")
    print("Simulations start as soon as each binary is ready.\n")

    completed_sims  = 0
    all_sim_futures = {}

    with ProcessPoolExecutor(max_workers=MAX_SIM_WORKERS) as sim_executor:
        with ThreadPoolExecutor(max_workers=1) as compile_executor:

            compile_futures = {
                compile_executor.submit(
                    compile_and_simulate,
                    config, config_filename, pref, exp_name, val,
                    sim_executor
                ): (pref, exp_name, val)
                for config, config_filename, pref, exp_name, val in all_configs
            }

            for compile_future in as_completed(compile_futures):
                pref, exp_name, val = compile_futures[compile_future]
                try:
                    for sf in compile_future.result():
                        all_sim_futures[sf] = (pref, exp_name, val)
                except Exception as e:
                    print(f"[ERROR] Compile {pref}/{exp_name}={val}: {e}")

            for sim_future in as_completed(all_sim_futures):
                completed_sims += 1
                pref, exp_name, val = all_sim_futures[sim_future]
                try:
                    msg = sim_future.result()
                except Exception as e:
                    msg = f"[ERROR] {pref}/{exp_name}={val}: {e}"
                print(f"[{completed_sims}/{total_sims}] {msg}")

    print(f"\nAll experiments completed in "
          f"{fmt_time(time.perf_counter() - wall_start)}.")
    print(f"Logs saved to the {RESULTS_DIR}/ directory.")


if __name__ == "__main__":
    main()