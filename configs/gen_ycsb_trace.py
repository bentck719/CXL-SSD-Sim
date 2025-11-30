import numpy as np
import argparse

# --- 參數設定 ---
parser = argparse.ArgumentParser(description="Generate YCSB-like memory traces for gem5")
parser.add_argument("--output", default="ycsb_trace.txt", help="Output trace file name")
parser.add_argument("--num-requests", type=int, default=100000, help="Total number of requests to generate")
parser.add_argument("--workload", type=str, default="A", choices=["A", "B", "C"], help="YCSB Workload type (A: 50/50, B: 95/5, C: 100/0)")
parser.add_argument("--mem-range", type=str, default="2GB", help="Memory range size (e.g., 2GB)")
parser.add_argument("--zipf-alpha", type=float, default=1.2, help="Zipfian parameter (higher = more skewed/hotter)")
args = parser.parse_args()

# 解析記憶體大小
def parse_size(size_str):
    if size_str.endswith("GB"): return int(size_str[:-2]) * 1024**3
    if size_str.endswith("MB"): return int(size_str[:-2]) * 1024**2
    if size_str.endswith("KB"): return int(size_str[:-2]) * 1024
    return int(size_str)

MAX_ADDR = parse_size(args.mem_range)
BLOCK_SIZE = 64  # Cache Line size aligned

# --- 設定讀寫比例 ---
# Workload A: Update heavy (50% Read, 50% Write)
# Workload B: Read mostly (95% Read, 5% Write)
# Workload C: Read only (100% Read)
if args.workload == "A":
    read_prob = 0.5
elif args.workload == "B":
    read_prob = 0.95
else:
    read_prob = 1.0

print(f"Generating {args.num_requests} requests for Workload {args.workload}...")
print(f"Memory Range: 0 - {MAX_ADDR} bytes")
print(f"Zipfian Alpha: {args.zipf_alpha} (Hotspot intensity)")

# --- 生成 Zipfian 分布 (模擬熱點) ---
# 我們假設記憶體被切分成很多個 4KB Page 或 64B Block 作為 Key
num_keys = MAX_ADDR // BLOCK_SIZE
# 使用 numpy 生成 Zipfian 分布的索引
# 注意：numpy 的 zipf 參數 a 必須 > 1
# 為了效能，我們生成一批隨機數，然後映射到地址
zipf_indices = np.random.zipf(a=args.zipf_alpha, size=args.num_requests)
# 取餘數確保落在地址空間內 (這是一種簡單的映射方式)
access_indices = zipf_indices % num_keys
addresses = access_indices * BLOCK_SIZE

# --- 寫入 Trace 檔 ---
# gem5 TrafficGen Trace Format:
# <r/w> <addr> <size> <tick_delta>
with open(args.output, "w") as f:
    current_tick = 0
    for i, addr in enumerate(addresses):
        # 決定讀或寫
        is_read = np.random.random() < read_prob
        cmd = 'r' if is_read else 'w'
        
        # 為了模擬高併發，Tick 間隔設短一點 (例如 10ns = 10000 ticks)
        # 這裡設為 0 表示我們希望 TrafficGen 盡快發送 (依賴 gem5 的 flow control)
        # 或者設一個固定延遲
        tick_delta = 1000  # 1ns if 1GHz clock
        
        # 寫入檔案
        f.write(f"{cmd} {int(addr)} {BLOCK_SIZE} {tick_delta}\n")

print(f"Done! Trace saved to {args.output}")