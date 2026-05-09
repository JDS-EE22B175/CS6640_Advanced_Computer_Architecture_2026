import os
import subprocess
from concurrent.futures import ThreadPoolExecutor

os.makedirs("results", exist_ok=True)

traces = ["T1.xz", "T2.xz", "T3.xz", "T4.xz"]
policies = ["adaptive", "mpp"]
binaryBase = "bin/champsim_{}"
traceDir = os.path.expanduser("traces")
maxWorkers = 6

def runSimulation(trace, policy):
    tracePath = os.path.join(traceDir, trace)
    traceName = trace.split('.')[0]
    outPath = f"results/{policy}_{traceName}.txt"
    binaryPath = binaryBase.format(policy)

    if os.path.exists(tracePath):
        with open(outPath, "w") as outFile:
            cmd = [
                binaryPath,
                "--warmup-instructions", "100000000",
                "--simulation-instructions", "500000000",
                tracePath
            ]
            print("Running:", cmd)
            subprocess.run(cmd, stdout=outFile, stderr=subprocess.STDOUT)

# Build all (trace, policy) combinations
jobs = [(trace, policy) for policy in policies for trace in traces]

with ThreadPoolExecutor(max_workers=maxWorkers) as executor:
    executor.map(lambda args: runSimulation(*args), jobs)
