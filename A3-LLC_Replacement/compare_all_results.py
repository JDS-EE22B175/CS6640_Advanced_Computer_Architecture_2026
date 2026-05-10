import os
import glob
import pandas as pd
import numpy as np
from scipy.stats import gmean

def compare_results(root_dir="."):
    all_data = []
    
    # Find all A3_Results.xlsx files recursively
    excel_files = glob.glob(os.path.join(root_dir, "**", "A3_Results.xlsx"), recursive=True)
    
    if not excel_files:
        print("Could not find any A3_Results.xlsx files in the current directory or subdirectories.")
        print("Please ensure your parse logs script has been run in the respective folders first.")
        return

    for file in excel_files:
        folder_path = os.path.dirname(file)
        folder_name = os.path.basename(folder_path)
        
        # Give root folder a recognizable name
        if folder_name == "." or folder_name == "":
            folder_name = "root"
            
        try:
            df = pd.read_excel(file)
            
            # Ensure required columns exist
            if "IPC" not in df.columns or "Trace" not in df.columns or "Replacement_Policy" not in df.columns:
                print(f"Skipping {file}: Missing required columns (IPC, Trace, Replacement_Policy).")
                continue
                
            for _, row in df.iterrows():
                # We only want T1-T4, but we'll accept any Trace starting with T
                if str(row["Trace"]).startswith("T"):
                    all_data.append({
                        "Folder_Path": folder_path,
                        "Folder_Name": folder_name,
                        "Policy": row["Replacement_Policy"],
                        "Trace": row["Trace"],
                        "IPC": row["IPC"]
                    })
        except Exception as e:
            print(f"Error reading {file}: {e}")
            
    if not all_data:
        print("No valid KPI data found in any Excel files.")
        return
        
    df = pd.DataFrame(all_data)
    
    # Pivot table to get traces as columns (T1, T2, T3, T4)
    # Using 'mean' aggfunc just in case there are duplicates in the same file
    pivot_df = df.pivot_table(
        index=["Folder_Path", "Folder_Name", "Policy"], 
        columns="Trace", 
        values="IPC",
        aggfunc="mean" 
    ).reset_index()
    
    # Identify the trace columns
    traces = sorted([c for c in pivot_df.columns if str(c).startswith("T")])
    
    # Calculate Geometric Mean, Sum of IPC, and Count of completed traces
    # Using gmean (Geometric Mean) is standard in computer architecture for normalized metrics
    pivot_df["GeoMean_IPC"] = pivot_df[traces].apply(lambda x: gmean(x.dropna()) if len(x.dropna()) > 0 else np.nan, axis=1)
    pivot_df["Sum_IPC"] = pivot_df[traces].sum(axis=1)
    pivot_df["Completed"] = pivot_df[traces].notna().sum(axis=1)
    
    # Sort by Most completed traces first, then highest Geometric Mean
    pivot_df = pivot_df.sort_values(by=["Completed", "GeoMean_IPC"], ascending=[False, False]).reset_index(drop=True)
    
    # 1-indexed rank
    pivot_df.index += 1
    pivot_df.index.name = "Rank"
    
    # Formatting outputs
    print("\n" + "="*100)
    print(" 🏆 CHAMP_SIM LLC REPLACEMENT POLICY LEADERBOARD 🏆")
    print("="*100)
    
    pd.set_option('display.max_columns', None)
    pd.set_option('display.width', 1000)
    pd.set_option('display.float_format', lambda x: f'{x:.4f}')
    
    display_cols = ["Folder_Name", "Policy", "GeoMean_IPC", "Sum_IPC"] + traces + ["Completed", "Folder_Path"]
    display_cols = [c for c in display_cols if c in pivot_df.columns]
    
    print(pivot_df[display_cols])
    print("="*100)
    
    # Save the leaderboard to a CSV file for easy external viewing
    output_csv = "Policy_Leaderboard.csv"
    pivot_df.to_csv(output_csv)
    print(f"\nLeaderboard successfully saved to '{output_csv}'")

if __name__ == "__main__":
    compare_results()
