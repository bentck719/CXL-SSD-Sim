import m5
from m5.objects import *
import argparse
import numpy as np

# --- 1. YCSB 流量生成器 (Python Generator) ---
def ycsb_generator(tgen, workload_type, num_requests, mem_size_bytes):
    """
    這是一個 Python Generator，它會直接被 gem5 的 PyTrafficGen 呼叫。
    我們在這裡即時計算 Zipfian 分佈，模擬 YCSB。
    """
    
    # 設定參數
    if workload_type == "A":
        read_prob = 0.5  # 50% Read, 50% Write
    elif workload_type == "B":
        read_prob = 0.95 # 95% Read, 5% Write
    else:
        read_prob = 1.0
        
    zipf_alpha = 1.1 # YCSB 標準熱點係數
    block_size = 64
    num_keys = mem_size_bytes // block_size
    
    print(f"Generating YCSB-{workload_type} traffic (PyTrafficGen)...")
    print(f"  Total Requests: {num_requests}")
    print(f"  Zipf Alpha: {zipf_alpha}")

    # 預先生成熱點分佈 (為了效能，一次生成一批)
    # 注意：Zipf 分佈的 tail 可能很長，我們取餘數來確保落在記憶體範圍內
    hot_indices = np.random.zipf(a=zipf_alpha, size=num_requests) % num_keys
    
    # 逐一發送請求給 gem5
    for i, idx in enumerate(hot_indices):
        addr = int(idx) * block_size
        
        # 決定讀或寫
        is_read = np.random.random() < read_prob
        
        # 模擬請求間隔 (Ticks)
        # 如果你希望壓力測試 (saturation)，設為 0 或很小
        # 如果希望模擬特定 IOPS，這裡要調整 (1GHz時, 1000 ticks = 1us)
        delta = 1000 

        if is_read:
            # 產生讀取請求
            yield tgen.createRead(addr, block_size, delta)
        else:
            # 產生寫入請求
            yield tgen.createWrite(addr, block_size, delta)
            
    # 結束模擬
    yield tgen.createExit(0)

# --- 2. 參數設定 ---
parser = argparse.ArgumentParser()
parser.add_argument("--host-cache-size", default="1GB", help="Host DRAM Cache Size")
parser.add_argument("--workload", default="A", choices=["A", "B"], help="YCSB Workload Type")
parser.add_argument("--requests", type=int, default=100000, help="Number of requests")
args = parser.parse_args()

# --- 3. 建立系統 ---
system = System()
system.clk_domain = SrcClockDomain(clock='1GHz', voltage_domain=VoltageDomain())
system.mem_mode = 'timing'
system.mem_ranges = [AddrRange('4GB')]

# CXL Device
try:
    system.cxl_ssd = CxlMemory()
    system.cxl_ssd.host_cache_size = args.host_cache_size
    system.cxl_ssd.range = AddrRange('2GB')
except NameError:
    print("Error: CxlMemory not found. Using SimpleMemory for testing script logic.")
    system.cxl_ssd = SimpleMemory(range=AddrRange('2GB'))

# [關鍵修改] 使用 PyTrafficGen 而不是 TrafficGen
# PyTrafficGen 允許 Python 腳本直接注入封包
system.tgen = PyTrafficGen()

system.membus = SystemXBar()

# 連接
system.tgen.port = system.membus.cpu_side_ports
system.cxl_ssd.pio = system.membus.mem_side_ports # 或 port, 看你的 C++ 定義
system.system_port = system.membus.cpu_side_ports

# --- 4. 執行模擬 ---
root = Root(full_system=False, system=system)
m5.instantiate()

# [關鍵步驟] 啟動 Traffic Generator
# 我們把 Python 函式餵給它
traffic_stream = ycsb_generator(system.tgen, args.workload, args.requests, 2*1024**3)
system.tgen.start(traffic_stream)

print(f"Simulation started. Host Cache: {args.host_cache_size}")

exit_event = m5.simulate()

print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")