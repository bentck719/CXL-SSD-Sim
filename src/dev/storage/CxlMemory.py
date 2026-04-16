from m5.SimObject import SimObject
from m5.params import *
from m5.objects.PciDevice import *


class CxlMemory(PciDevice):
    type = 'CxlMemory'
    cxx_header = "dev/storage/cxl_memory.hh"
    cxx_class = 'gem5::CxlMemory'

    # ── Byte-path latency budget ──────────────────────────────────────────
    # L_byte target: 212–326 ns  (COBRA model)
    #   cxl_mem_latency : PCIe traversal + CXL.mem handshake overhead = 250 ns
    #   latency         : DRAM access at the CXL-SSD device side       =  50 ns
    #   BAR2 register reads return cxl_mem_latency only             = 250 ns  ✓
    #   BAR0 data reads/writes on DRAM cache hit: 250 + 50           = 300 ns  ✓
    latency         = Param.Latency('50ns',   "DRAM access latency at the CXL-SSD device")
    cxl_mem_latency = Param.Latency('250ns',  "PCIe traversal + CXL.mem protocol overhead (L_byte component)")

    # ── Block-path latency budget ─────────────────────────────────────────
    # L_block target: 2280 ns PCIe DMA setup + SimpleSSD NAND timing
    # Added to every NAND operation (dirty-page eviction, GC flush, cache miss).
    pcie_latency    = Param.Latency('2280ns', "PCIe DMA setup/transfer overhead for NAND block operations")
    # evict_strategy = Param.String("TwoQ", "cxl cache evict strategy, Direct LRU FIFO TwoQ LFRU")

    VendorID = 0x8086
    DeviceID = 0x7890
    Command = 0x0
    Status = 0x280
    Revision = 0x0
    ClassCode = 0x01
    SubClassCode = 0x01
    ProgIF = 0x85
    InterruptLine = 0x1f
    InterruptPin = 0x01

    # BAR0/BAR1: 64-bit 4GiB data region (sector writes from cobra_cxl_write_sector)
    BAR0 = PciMemBar(size='4GiB')
    BAR1 = PciMemUpperBar()

    # BAR2: 4KiB MMIO control page — BWB status registers (Log_Tail, Log_Head, fill level)
    BAR2 = PciMemBar(size='4KiB')
