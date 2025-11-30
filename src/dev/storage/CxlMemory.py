from m5.SimObject import SimObject
from m5.params import *
from m5.objects.Device import DmaDevice


class CxlMemory(DmaDevice):
    type = 'CxlMemory'
    cxx_header = "dev/storage/cxl_memory.hh"
    cxx_class = 'gem5::CxlMemory'

    latency = Param.Latency('50ns', "DRAM latency for cxl-SSD")
    cxl_mem_latency = Param.Latency('25ns', "cxl.mem protocol processing's latency for device")
    host_cache_size = Param.MemorySize("1GB", "Size of the simulated Host DRAM Page Cache")

    threshold_isolated = Param.UInt8(4, "Threshold for Isolated Hotspot detection")
    threshold_distributed = Param.UInt8(4, "Threshold for Distributed Hotspot detection")
