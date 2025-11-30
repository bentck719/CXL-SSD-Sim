import m5
from m5.objects import *
import argparse
import os

# --- 1. 自動產生 TrafficGen 設定檔 (已修正邏輯) ---
def create_ycsb_cfg(filename, workload_type, ssd_size_bytes):
    """
    建立一個模擬 YCSB 的狀態機設定檔。
    """
    # YCSB 讀寫比例
    if workload_type == "A":
        read_percent = 50
    elif workload_type == "B":
        read_percent = 95
    else:
        read_percent = 100

    # 定義熱點 (Hotspot): 20% 的空間承受 80% 的流量
    hot_ratio = 0.2
    
    hot_limit = int(ssd_size_bytes * hot_ratio)
    total_limit = int(ssd_size_bytes)
    
    block_size = 64
    
    # 這裡的 duration 設短一點 (10us)，讓狀態切換更頻繁，混合度更好
    state_duration = "10000000" 

    print(f"--- Generating Config: {filename} ---")
    
    with open(filename, "w") as f:
        f.write("INIT 0\n")
        # State 0: Initial Idle
        f.write("STATE 0 1000000 IDLE\n")
        
        # State 1: Hot Region Access (熱區 0 ~ 20%)
        f.write(f"STATE 1 {state_duration} RANDOM {read_percent} 0 {hot_limit} {block_size}\n")
        
        # State 2: Cold Region Access (冷區 20% ~ 100%)
        f.write(f"STATE 2 {state_duration} RANDOM {read_percent} {hot_limit} {total_limit} {block_size}\n")
        
        # --- Transition Logic (修正後) ---
        # 初始狀態：80% 機率進熱區, 20% 進冷區
        f.write(f"TRANSITION 0 1 0.8\n")
        f.write(f"TRANSITION 0 2 0.2\n")
        
        # State 1 (熱區) 結束後：
        # 80% 留在熱區繼續讀寫, 20% 跳去冷區
        f.write(f"TRANSITION 1 1 0.8\n")
        f.write(f"TRANSITION 1 2 0.2\n")
        
        # State 2 (冷區) 結束後：
        # 80% 跳回熱區 (因為熱區流量大), 20% 留在冷區
        f.write(f"TRANSITION 2 1 0.8\n")
        f.write(f"TRANSITION 2 2 0.2\n")

# --- 2. 參數設定 ---
parser = argparse.ArgumentParser()
parser.add_argument("--workload", default="A", choices=["A", "B"], help="YCSB Workload")
parser.add_argument("--requests", type=int, default=100000, help="Request count")
parser.add_argument("--host-cache-size", default="1GB", help="Host Cache Size")
# 加入 duration 參數，預設跑 1ms (10^9 ticks)
parser.add_argument("--duration", type=int, default=100000000000000000, help="Simulation duration in ticks")

args = parser.parse_args()

ssd_size_bytes = 4 * 1024**3 # 4GB

cfg_filename = f"ycsb_{args.workload.lower()}_smart.cfg"
create_ycsb_cfg(cfg_filename, args.workload, ssd_size_bytes)

# --- 3. 建立系統基礎 ---
system = System()
system.clk_domain = SrcClockDomain(clock='1GHz', voltage_domain=VoltageDomain())
system.mem_mode = 'timing'
system.mem_ranges = [] 
system.membus = SystemXBar()

# --- 4. 實例化 CXL SSD 與 CommMonitor ---
try:
    system.cxl_ssd = CxlMemory()
    system.cxl_ssd.host_cache_size = args.host_cache_size
    
    # [新增] 建立 CommMonitor
    system.ssd_monitor = CommMonitor()
    
    system.ssd_monitor.latency_bins = 20
    system.ssd_monitor.bandwidth_bins = 20

    # 1. 將 Monitor 的 "CPU側 (入口)" 接到 Bus 上
    #    TrafficGen 的請求會經過 Bus -> 流入 Monitor
    system.membus.mem_side_ports = system.ssd_monitor.cpu_side_port
    
    # 2. 將 Monitor 的 "Memory側 (出口)" 接到 SSD 的 'pio' Port
    #    因為你的 C++ Code 是透過 pio port 接收 read/write request
    system.ssd_monitor.mem_side_port = system.cxl_ssd.pio
    
    # 3. SSD 的 'dma' Port (Master) 接回 Bus
    #    雖然你在這次 TrafficGen 測試中可能主要是被動接收請求，
    #    但 DmaDevice 必須把 dma port 接上才能運作 (即使沒用到)。
    #    這條線是讓 SSD 主動去讀 DRAM 用的，所以接到 Bus 的 cpu_side。
    system.cxl_ssd.dma = system.membus.cpu_side_ports

except NameError:
    print("Error: CxlMemory object not found. Make sure you have compiled gem5 with your custom object.")
    exit(1)

# --- 5. Traffic Generator ---
system.tgen = TrafficGen()
system.tgen.config_file = cfg_filename
# TGen 接到 Bus 的 CPU 側
system.tgen.port = system.membus.cpu_side_ports

# System port 用來載入 kernel 等，這裡沒用 FS mode，但通常還是要接
system.system_port = system.membus.cpu_side_ports

# --- 6. 執行 ---
root = Root(full_system=False, system=system)
m5.instantiate()

print(f"--- Simulation Started ---")
print(f"Using config: {cfg_filename}")
print(f"Monitor enabled. Check 'system.ssd_monitor' in stats.txt after run.")

# 執行指定的 ticks
exit_event = m5.simulate(args.duration)

print(f"Exiting @ tick {m5.curTick()} cause: {exit_event.getCause()}")