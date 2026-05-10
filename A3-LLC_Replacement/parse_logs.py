import os
import re
import glob
import pandas as pd
import numpy as np
from pathlib import Path
from scipy.stats import gmean

def parse_champsim_logs(folder_path=".", output_excel="A3_Results.xlsx"):
    # Regex patterns for General KPIs
    ipc_pattern = re.compile(r"Simulation complete CPU \d+ instructions: (\d+) cycles: (\d+) cumulative IPC: ([\d\.]+)")
    branch_pattern = re.compile(r"CPU \d+ Branch Prediction Accuracy: ([\d\.]+)% MPKI: ([\d\.]+)")
    
    # Regex patterns for LLC Info
    llc_total_pattern = re.compile(r"cpu0->LLC TOTAL\s+ACCESS:\s+(\d+)\s+HIT:\s+(\d+)\s+MISS:\s+(\d+)")
    llc_load_pattern = re.compile(r"cpu0->LLC LOAD\s+ACCESS:\s+(\d+)\s+HIT:\s+(\d+)\s+MISS:\s+(\d+)")
    llc_rfo_pattern = re.compile(r"cpu0->LLC RFO\s+ACCESS:\s+(\d+)\s+HIT:\s+(\d+)\s+MISS:\s+(\d+)")
    llc_write_pattern = re.compile(r"cpu0->LLC WRITE\s+ACCESS:\s+(\d+)\s+HIT:\s+(\d+)\s+MISS:\s+(\d+)")
    llc_latency_pattern = re.compile(r"cpu0->LLC AVERAGE MISS LATENCY:\s+([\d\.]+)\s+cycles")
    
    data = []
    
    # Find all .txt files in the target directory recursively
    search_path = os.path.join(folder_path, "replacement", "**", "*.txt")
    for filepath in glob.glob(search_path, recursive=True):
        path_obj = Path(filepath)
        filename = path_obj.stem
        
        # Determine the policy name from the directory structure
        # Assuming format: replacement/<policy>/logs/<file>.txt
        # If logs folder exists, policy is parent.parent, else it's parent
        if path_obj.parent.name == "logs":
            policy = path_obj.parent.parent.name
        else:
            policy = path_obj.parent.name
            
        # Extract Trace Name
        parts = filename.split('_')
        trace = parts[-1] if len(parts) > 1 else "Unknown"
        
        # Initialize data dictionary for the current file
        sim_data = {
            "Filename": filename,
            "Replacement_Policy": policy,
            "Trace": trace,
            "Instructions": None,
            "Cycles": None,
            "IPC": None,
            "Branch_Pred_Accuracy_%": None,
            "Branch_MPKI": None,
            "LLC_Total_Access": None,
            "LLC_Total_Hit": None,
            "LLC_Total_Miss": None,
            "LLC_Load_Access": None,
            "LLC_Load_Hit": None,
            "LLC_Load_Miss": None,
            "LLC_RFO_Access": None,
            "LLC_Write_Access": None,
            "LLC_Avg_Miss_Latency": None,
            "Final_Policy_Stats": ""
        }
        
        with open(filepath, 'r', encoding='utf-8') as f:
            lines = f.readlines()
            
        dram_stats_passed = False
        custom_stats = []
        
        for line in lines:
            # 1. Parse Core KPIs
            ipc_match = ipc_pattern.search(line)
            if ipc_match:
                sim_data["Instructions"] = int(ipc_match.group(1))
                sim_data["Cycles"] = int(ipc_match.group(2))
                sim_data["IPC"] = float(ipc_match.group(3))
                
            branch_match = branch_pattern.search(line)
            if branch_match:
                sim_data["Branch_Pred_Accuracy_%"] = float(branch_match.group(1))
                sim_data["Branch_MPKI"] = float(branch_match.group(2))
                
            # 2. Parse LLC Info
            llc_tot_match = llc_total_pattern.search(line)
            if llc_tot_match:
                sim_data["LLC_Total_Access"] = int(llc_tot_match.group(1))
                sim_data["LLC_Total_Hit"] = int(llc_tot_match.group(2))
                sim_data["LLC_Total_Miss"] = int(llc_tot_match.group(3))
                
            llc_ld_match = llc_load_pattern.search(line)
            if llc_ld_match:
                sim_data["LLC_Load_Access"] = int(llc_ld_match.group(1))
                sim_data["LLC_Load_Hit"] = int(llc_ld_match.group(2))
                sim_data["LLC_Load_Miss"] = int(llc_ld_match.group(3))
                
            llc_rfo_match = llc_rfo_pattern.search(line)
            if llc_rfo_match:
                sim_data["LLC_RFO_Access"] = int(llc_rfo_match.group(1))
                
            llc_wr_match = llc_write_pattern.search(line)
            if llc_wr_match:
                sim_data["LLC_Write_Access"] = int(llc_wr_match.group(1))
                
            llc_lat_match = llc_latency_pattern.search(line)
            if llc_lat_match:
                sim_data["LLC_Avg_Miss_Latency"] = float(llc_lat_match.group(1))
                
            # 3. Parse Custom Replacement Policy Stats at the EOF
            if "DRAM Statistics" in line:
                dram_stats_passed = True
            elif dram_stats_passed:
                # Ignore standard DRAM output lines to isolate custom trailing stats
                if not any(keyword in line for keyword in ["Channel", "ROW_BUFFER", "AVG DBUS", "REFRESHES", "FULL"]):
                    clean_line = line.strip()
                    if clean_line:  # If line is not empty
                        custom_stats.append(clean_line)
                
        # Join all captured custom stats into a single readable string
        sim_data["Final_Policy_Stats"] = " | ".join(custom_stats)
        data.append(sim_data)
        
    # Generate DataFrame and export to Excel
    df = pd.DataFrame(data)
    if df.empty:
        print("No log files found or no data extracted.")
        return

    df.sort_values(by=["Trace", "Replacement_Policy"], inplace=True)
    df.to_excel(output_excel, index=False)
    print(f"Successfully processed {len(data)} files.")
    print(f"Master data saved to: {output_excel}")

    # --- GENERATE LEADERBOARDS ---
    # We only care about Trace and IPC for the leaderboards
    valid_traces = df[df["Trace"].str.startswith("T", na=False)]
    
    if valid_traces.empty:
        return
        
    pivot_df = valid_traces.pivot_table(
        index="Replacement_Policy", 
        columns="Trace", 
        values="IPC",
        aggfunc="mean" 
    ).reset_index()
    
    traces = sorted([c for c in pivot_df.columns if str(c).startswith("T")])
    
    # Calculate Metrics
    pivot_df["GeoMean_IPC"] = pivot_df[traces].apply(lambda x: gmean(x.dropna()) if len(x.dropna()) > 0 else np.nan, axis=1)
    pivot_df["Sum_IPC"] = pivot_df[traces].sum(axis=1)
    pivot_df["Completed"] = pivot_df[traces].notna().sum(axis=1)
    
    pivot_df = pivot_df.sort_values(by=["Completed", "GeoMean_IPC"], ascending=[False, False]).reset_index(drop=True)
    pivot_df.index += 1
    pivot_df.index.name = "Rank"
    
    # Display and Save Global Leaderboard
    print("\n" + "="*80)
    print(" 🏆 GLOBAL LLC REPLACEMENT POLICY LEADERBOARD 🏆")
    print("="*80)
    
    pd.set_option('display.max_columns', None)
    pd.set_option('display.width', 1000)
    pd.set_option('display.float_format', lambda x: f'{x:.4f}')
    
    display_cols = ["Replacement_Policy", "GeoMean_IPC", "Sum_IPC"] + traces + ["Completed"]
    display_cols = [c for c in display_cols if c in pivot_df.columns]
    
    print(pivot_df[display_cols])
    print("="*80)
    
    pivot_df.to_csv("Policy_Leaderboard.csv")
    print("\nGlobal Leaderboard saved to 'Policy_Leaderboard.csv'")

if __name__ == "__main__":
    parse_champsim_logs()