#include <cstdio>

#include "base/trace.hh"
#include "dev/storage/cxl_memory.hh"
#include "debug/CxlMemory.hh"
#include "debug/CxlMemoryCoherency.hh"

namespace gem5 {

/**
 * eventEngine global variable
 */
Engine engine;

std::ostream *pDebugLog = nullptr;
SimpleSSD::ConfigReader ssdConfig =
    initSimpleSSDEngine(&engine, pDebugLog, pDebugLog,
                        "./src/dev/storage/simplessd/config/sample.cfg");

EvictStrategy *Worker(EvictStrategyMode mode, size_t capacity) {
  if (mode == EvictStrategyMode::Direct) {
    return new DirectEvictStrategy(capacity / CXL_SSD_PAGE_SIZE);
  } else if (mode == EvictStrategyMode::LRU) {
    return new LRUEvictStrategy(capacity);
  } else if (mode == EvictStrategyMode::FIFO) {
    return new FIFOEvictStrategy(capacity);
  } else if (mode == EvictStrategyMode::TwoQ) {
    return new TwoQEvictStrategy(capacity);
  } else if (mode == EvictStrategyMode::LFRU) {
    return new LFRUEviceStrategy(capacity);
  }
  assert(0);
}

/**
 * CxlMemory
 */

CxlMemory::CxlMemory(const Param &p)
    : PciDevice(p), latency_(p.latency), cxl_mem_latency_(p.cxl_mem_latency),
      pHIL(new SimpleSSD::HIL::HIL(ssdConfig)) {
  data_fd_ = open("./CxlSSD.img", O_RDWR | O_CREAT | O_TRUNC, 0666);
  if (data_fd_ == -1) {
    perror("Error opening file");
    assert(0);
  }
  if (ftruncate(data_fd_, CXL_SSD_CAPACITY) == -1) {
    perror("Error setting file size");
    assert(0);
  }

  mapped_cache_ = (char *)mmap(NULL, CXL_SSD_CAPACITY, PROT_READ | PROT_WRITE,
                               MAP_SHARED, data_fd_, 0);

  if (mapped_cache_ == MAP_FAILED) {
    perror("Error mmap CxlSSD.img");
    assert(0);
  }
  pages = new Page[pages_counts];
  evict_strategy = Worker(EvictStrategyMode::FIFO, cache_capacity);
}

CxlMemory::~CxlMemory() {
  delete pHIL;
  delete[] pages;
  delete evict_strategy;

  if (munmap(mapped_cache_, CXL_SSD_CAPACITY) == -1) {
    perror("Error unmapping file from memory");
  }
  close(data_fd_);
}

uint8_t *CxlMemory::toHostAddr(Addr addr) {
  Addr ssd_start = physicalAddrToSSDAddr(addr);
  return (uint8_t *)mapped_cache_ + ssd_start;
}

bool CxlMemory::checkAndUpdatePageCache(uint64_t logical_frame, uint64_t ssd_start) {
  uint64_t index = evict_strategy->access(logical_frame);
  auto &page = pages[index];

  DPRINTF(CxlMemory, "checkAndUpdatePageCache: logical_frame=%lx, index=%lx\n", 
          logical_frame, index);

  // Check for cache hit
  if (page.IsValid() && page.CacheHit(ssd_start)) {
    cache_hit_counts_ += 1;
    return true;  // Cache hit
  }

  // Cache miss - need to handle dirty page writeback
  if (page.IsDirty()) {
    performPageWriteback(index);
  }

  // Update page metadata for new data
  page.SetValid();
  page.SetTag(ssd_start);
  
  return false;  // Cache miss
}

Tick CxlMemory::performPageWriteback(uint64_t page_index) {
  auto &page = pages[page_index];
  uint64_t dirty_addr_start = page.tag_;
  uint64_t dirty_page_size = logical_page_size_;

  SSDRequestBuilder builder(instruction_id, pHIL, logical_page_size_);
  uint64_t write_latency = builder.issueWriteRequest(dirty_addr_start, dirty_page_size);

  page.ClearDirty();
  
  DPRINTF(CxlMemory, "performPageWriteback: addr=%lx, size=%lx, latency=%ld\n",
          dirty_addr_start, dirty_page_size, write_latency);

  // TODO: need fix this patch, get real latency from SimpleSSD
  return 35250000 + write_latency * 10;
}

uint64_t CxlMemory::executeReadRequest(uint64_t ssd_start, uint64_t size) {
  SSDRequestBuilder builder(instruction_id, pHIL, logical_page_size_);
  uint64_t read_latency = builder.issueReadRequest(ssd_start, size);

  DPRINTF(CxlMemory, "executeReadRequest: ssd_start=%lx, size=%lx, latency=%ld\n",
          ssd_start, size, read_latency);

  return read_latency * 10;
}

uint64_t CxlMemory::executeWriteRequest(uint64_t ssd_start, uint64_t size) {
  SSDRequestBuilder builder(instruction_id, pHIL, logical_page_size_);
  uint64_t write_latency = builder.issueWriteRequest(ssd_start, size);

  DPRINTF(CxlMemory, "executeWriteRequest: ssd_start=%lx, size=%lx, latency=%ld\n",
          ssd_start, size, write_latency);

  return write_latency * 10;
}

Tick CxlMemory::read(PacketPtr pkt) {
  DPRINTF(CxlMemory, "read address : (%lx, %lx)\n", pkt->getAddr(),
          pkt->getSize());
  DPRINTF(CxlMemoryCoherency, "read packet: MemCmd %s, address %lx\n",
          pkt->cmdString(), pkt->getAddr());
  access_counts_ += 1;

  access(pkt); // storage may be dram or SSD
  Tick cxl_latency = resolve_cxl_mem(pkt);

#ifdef CXL_MEMORY_ENABLE
  Tick storage_latency = latency_;
#else
  Tick storage_latency = ssdRead(pkt);
#endif

  DPRINTF(CxlMemory, "cxl_latency: %ld, read_latency: %ld\n", cxl_latency,
          storage_latency);

  return cxl_latency + storage_latency;
}

Tick CxlMemory::write(PacketPtr pkt) {
  DPRINTF(CxlMemory, "write address : (%lx, %lx)\n", pkt->getAddr(),
          pkt->getSize());
  DPRINTF(CxlMemoryCoherency,
          "write packet: MemCmd %s, address: %lx, req->isClean: %d, "
          "req->isInvalidate: %d\n",
          pkt->cmdString(), pkt->getAddr(), pkt->req->isCacheClean(),
          pkt->req->isCacheInvalidate());
  access_counts_ += 1;
  access(pkt);
  Tick cxl_latency = resolve_cxl_mem(pkt);

#ifdef CXL_MEMORY_ENABLE
  Tick storage_latency = latency_;
#else
  Tick storage_latency = ssdWrite(pkt);
#endif

  DPRINTF(CxlMemory, "cxl_latency: %ld, write_latency: %ld\n", cxl_latency,
          storage_latency);
  return cxl_latency + storage_latency;
}

AddrRangeList CxlMemory::getAddrRanges() const {
  return PciDevice::getAddrRanges();
}

Tick CxlMemory::resolve_cxl_mem(PacketPtr pkt) {
  if (pkt->cmd == MemCmd::ReadReq) {
    assert(pkt->isRead());
    assert(pkt->needsResponse());
  } else if (pkt->cmd == MemCmd::WriteReq) {
    assert(pkt->isWrite());
    assert(pkt->needsResponse());
  }
  return cxl_mem_latency_;
}

// process data
void CxlMemory::access(PacketPtr pkt) {
  // get the bar address when linux wirte the corresponding registers
  range_ = AddrRange(BARs[0]->addr(), BARs[0]->addr() + BARs[0]->size());
  // DPRINTF(CxlMemory, "range_ addr_ : %lx, size: %lx\n", range_.start(),
  // range_.size());
  if (pkt->cacheResponding()) {
    DPRINTF(CxlMemory, "Cache responding to %#llx: not responding\n",
            pkt->getAddr());
    return;
  }

  if (pkt->cmd == MemCmd::CleanEvict || pkt->cmd == MemCmd::WritebackClean) {
    DPRINTF(CxlMemory, "CleanEvict  on 0x%x: not responding\n", pkt->getAddr());
    return;
  }

  assert(pkt->getAddrRange().isSubset(range_));

  uint8_t *host_addr = toHostAddr(pkt->getAddr());

  if (pkt->cmd == MemCmd::SwapReq) {
    if (pkt->isAtomicOp()) {
      if (mapped_cache_) {
        pkt->setData(host_addr);
        (*(pkt->getAtomicOp()))(host_addr);
      }
    } else {
      std::vector<uint8_t> overwrite_val(pkt->getSize());
      uint64_t condition_val64;
      uint32_t condition_val32;

      if (!mapped_cache_) {
        panic("Swap only works if there is real memory "
              "(i.e. null=False)");
      }

      bool overwrite_mem = true;
      // keep a copy of our possible write value, and copy what is at the
      // memory address into the packet
      pkt->writeData(&overwrite_val[0]);
      pkt->setData(host_addr);

      if (pkt->req->isCondSwap()) {
        if (pkt->getSize() == sizeof(uint64_t)) {
          condition_val64 = pkt->req->getExtraData();
          overwrite_mem =
              !std::memcmp(&condition_val64, host_addr, sizeof(uint64_t));
        } else if (pkt->getSize() == sizeof(uint32_t)) {
          condition_val32 = (uint32_t)pkt->req->getExtraData();
          overwrite_mem =
              !std::memcmp(&condition_val32, host_addr, sizeof(uint32_t));
        } else
          panic("Invalid size for conditional read/write\n");
      }

      if (overwrite_mem)
        std::memcpy(host_addr, &overwrite_val[0], pkt->getSize());

      assert(!pkt->req->isInstFetch());
    }
  } else if (pkt->isRead()) {
    assert(!pkt->isWrite());
    if (mapped_cache_) {
      pkt->setData(host_addr);
    }
  } else if (pkt->isWrite()) {
    if (mapped_cache_) {
      pkt->writeData(host_addr);
    }
    assert(!pkt->req->isInstFetch());
  } else {
    panic("Unexpected packet %s", pkt->print());
  }

  if (pkt->needsResponse()) {
    pkt->makeResponse();
  }
}

bool CxlMemory::ssdAddrCheck(PacketPtr &pkt) {
  Addr pktStart = pkt->getAddr();
  Addr pktend = pkt->getAddr() + pkt->getSize();
  return range_.start() <= pktStart &&
         pktend <= (range_.start() + range_.size());
}

uint64_t CxlMemory::GetPagesIndex(Addr ssd_start) const {
  uint64_t tag = ssd_start / logical_page_size_;
  uint64_t index = tag & (pages_counts - 1);
  return index;
}

Tick CxlMemory::ssdRead(PacketPtr pkt) {
  if (!ssdAddrCheck(pkt)) {
    assert(0);
  }

  uint64_t ssd_start = physicalAddrToSSDAddr(pkt->getAddr());

#ifndef CXL_SSD_NO_CACHE
  uint64_t logical_frame = ssd_start & (~(logical_page_size_ - 1));
  
  // Check cache and update page metadata
  bool is_cache_hit = checkAndUpdatePageCache(logical_frame, ssd_start);
  
  if (is_cache_hit) {
    DPRINTF(CxlMemory, "ssd_read: Cache HIT at %lx\n", ssd_start);
    return latency_;
  }
  
  DPRINTF(CxlMemory, "ssd_read: Cache MISS at %lx\n", ssd_start);
#endif

  // Execute the actual SSD read
  uint64_t read_latency = executeReadRequest(ssd_start, pkt->getSize());

  DPRINTF(CxlMemory, "ssdread latency %ld, engine current tick %ld\n",
          read_latency, engine.getCurrentTick());

  return read_latency + latency_;
}

Tick CxlMemory::ssdWrite(PacketPtr pkt) {
  if (!ssdAddrCheck(pkt)) {
    assert(0);
  }

  uint64_t ssd_start = physicalAddrToSSDAddr(pkt->getAddr());

#ifndef CXL_SSD_NO_CACHE
  uint64_t logical_frame = ssd_start & (~(logical_page_size_ - 1));
  
  // Check cache and update page metadata
  bool is_cache_hit = checkAndUpdatePageCache(logical_frame, ssd_start);
  
  if (is_cache_hit) {
    DPRINTF(CxlMemory, "ssd_write: Cache HIT at %lx\n", ssd_start);
    return latency_;
  }

  DPRINTF(CxlMemory, "ssd_write: Cache MISS at %lx\n", ssd_start);
  
  // Mark page as dirty for write
  auto &page = pages[evict_strategy->access(logical_frame)];
  page.SetDirty();

#endif // CXL_SSD_NO_CACHE

  // Execute the actual SSD write
  uint64_t write_latency = executeWriteRequest(ssd_start, pkt->getSize());

  DPRINTF(CxlMemory, "ssdwrite latency %ld, engine current tick %ld\n",
          write_latency, engine.getCurrentTick());

  return write_latency + latency_;
}

} // namespace gem5