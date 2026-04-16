#include "dev/storage/cxl_memory.hh"
#include "base/trace.hh"
#include "debug/CxlMemory.hh"
#include "debug/CxlMemoryCoherency.hh"
#include <cstdio>

namespace gem5 {

/**
 * eventEngine global variable
 */
Engine engine;

std::ostream *pDebugLog = nullptr;
SimpleSSD::ConfigReader ssdConfig =
    initSimpleSSDEngine(&engine, pDebugLog, pDebugLog,
                        "./src/dev/storage/simplessd/config/sample.cfg");
/**
 * eventengine for simplessd
 */
Engine::Engine()
    : SimpleSSD::Simulator(), simTick(0), counter(0), eventHandled(0) {}

Engine::~Engine() {}

bool Engine::insertEvent(SimpleSSD::Event eid, uint64_t tick,
                         uint64_t *pOldTick) {
  bool found = false;
  bool flag = false;
  auto old = eventQueue.begin();
  auto insert = eventQueue.end();

  for (auto iter = eventQueue.begin(); iter != eventQueue.end(); iter++) {
    if (iter->first == eid) {
      found = true;
      old = iter;

      if (pOldTick) {
        *pOldTick = iter->second;
      }
    }

    if (iter->second > tick && !flag) {
      insert = iter;
      flag = true;
    }
  }

  if (found && pOldTick) {
    if (*pOldTick == tick) {
      // Rescheduling to same tick. Ignore.
      return false;
    }
  }

  // Iterator will not invalidated on insert
  // Do insert first
  eventQueue.insert(insert, {eid, tick});

  if (found) {
    eventQueue.erase(old);
  }

  return found;
}

bool Engine::removeEvent(SimpleSSD::Event eid) {
  bool found = false;

  for (auto iter = eventQueue.begin(); iter != eventQueue.end(); iter++) {
    if (iter->first == eid) {
      eventQueue.erase(iter);
      found = true;

      break;
    }
  }

  return found;
}

bool Engine::isEventExist(SimpleSSD::Event eid, uint64_t *pTick) {
  for (auto &iter : eventQueue) {
    if (iter.first == eid) {
      if (pTick) {
        *pTick = iter.second;
      }

      return true;
    }
  }

  return false;
}

uint64_t Engine::getCurrentTick() { return simTick; }

SimpleSSD::Event Engine::allocateEvent(SimpleSSD::EventFunction func) {
  auto iter = eventList.insert({++counter, func});

  if (!iter.second) {
    SimpleSSD::ssd_panic("Fail to allocate event");
  }

  return counter;
}

void Engine::scheduleEvent(SimpleSSD::Event eid, uint64_t tick) {
  auto iter = eventList.find(eid);

  if (iter != eventList.end()) {
    uint64_t tickCopy;

    tickCopy = simTick;

    if (tick < tickCopy) {
      SimpleSSD::ssd_warn("Tried to schedule %" PRIu64
                          " < simTick to event %" PRIu64
                          ". Set tick as simTick.",
                          tick, eid);

      tick = tickCopy;
    }

    uint64_t oldTick;

    if (insertEvent(eid, tick, &oldTick)) {
      SimpleSSD::ssd_warn("Event %" PRIu64 " rescheduled from %" PRIu64
                          " to %" PRIu64,
                          eid, oldTick, tick);
    }
  } else {
    SimpleSSD::ssd_panic("Event %" PRIu64 " does not exists", eid);
  }
}

void Engine::descheduleEvent(SimpleSSD::Event eid) {
  auto iter = eventList.find(eid);

  if (iter != eventList.end()) {
    removeEvent(eid);
  } else {
    SimpleSSD::ssd_panic("Event %" PRIu64 " does not exists", eid);
  }
}

bool Engine::isScheduled(SimpleSSD::Event eid, uint64_t *pTick) {
  bool ret = false;
  auto iter = eventList.find(eid);

  if (iter != eventList.end()) {
    ret = isEventExist(eid, pTick);
  } else {
    SimpleSSD::ssd_panic("Event %" PRIu64 " does not exists", eid);
  }

  return ret;
}

void Engine::deallocateEvent(SimpleSSD::Event eid) {
  auto iter = eventList.find(eid);

  if (iter != eventList.end()) {
    removeEvent(eid);
    eventList.erase(iter);
  } else {
    SimpleSSD::ssd_panic("Event %" PRIu64 " does not exists", eid);
  }
}

EvictStrategy *Worker(EvictStrategyMode mode, uint64_t capacity) {
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
      pcie_latency_(p.pcie_latency),
      pHIL(new SimpleSSD::HIL::HIL(ssdConfig)),
      gcEvent_([this]{ runGcStep(); }, name() + ".gcEvent") {
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
  evict_strategy = Worker(EvictStrategyMode::TwoQ, cache_capacity);

  /* ── Byte-Write Buffer allocation ─────────────────────────────────
   *
   * bwb_data_: 256MB contiguous ring buffer.  Zero-initialised so that
   * unwritten slots read back as zeros rather than stale host data.
   *
   * bwb_meta_: 4-way set-associative metadata array.  Value-initialised
   * (all fields zero / valid=false) so every slot starts as empty.
   * ─────────────────────────────────────────────────────────────── */
  bwb_data_ = new uint8_t[BWB_CAPACITY]();
  bwb_meta_ = new BwbMetaSet[BWB_META_SETS]();

  DPRINTF(CxlMemory,
    "[BWB] Initialised: %u slots × %u B = %lu MB ring buffer, "
    "%u-way × %u-set metadata array\n",
    BWB_MAX_SLOTS, BWB_PAGE_SIZE, BWB_CAPACITY >> 20,
    BWB_META_WAYS, BWB_META_SETS);
}

CxlMemory::~CxlMemory() {
  if (gcEvent_.scheduled())
    deschedule(&gcEvent_);

  delete pHIL;
  delete[] pages;
  delete evict_strategy;
  delete[] bwb_data_;
  delete[] bwb_meta_;

  if (munmap(mapped_cache_, CXL_SSD_CAPACITY) == -1) {
    perror("Error unmapping file from memory");
  }
  close(data_fd_);
}

void CxlMemory::startup() {
  PciDevice::startup();
  scheduleGcPoll();
  DPRINTF(CxlMemory, "[BWB-GC] Background GC scheduled (poll every %lu ticks, "
          "HWM=%u LWM=%u slots)\n",
          BWB_GC_POLL_TICKS, BWB_GC_HIGH_WM, BWB_GC_LOW_WM);
}

uint8_t *CxlMemory::toHostAddr(Addr addr) {
  Addr ssd_start = physicalAddrToSSDAddr(addr);
  return (uint8_t *)mapped_cache_ + ssd_start;
}

Tick CxlMemory::read(PacketPtr pkt) {
  DPRINTF(CxlMemory, "read address : (%lx, %lx)\n", pkt->getAddr(),
          pkt->getSize());
  DPRINTF(CxlMemoryCoherency, "read packet: MemCmd %s, address %lx\n",
          pkt->cmdString(), pkt->getAddr());
  access_counts_ += 1;

  /* ── BAR2: BWB MMIO control register read ─────────────────────────
   * If the address falls within the 4 KiB BAR2 window, decode the
   * register offset and return the requested BWB status value.
   * The BAR addresses are only valid after the OS programs config space,
   * so we guard on size() > 0.
   * ─────────────────────────────────────────────────────────────── */
  if (BARs[2]->size() > 0 && BARs[2]->range().contains(pkt->getAddr())) {
    return handleBar2Read(pkt);
  }

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

  /* ── BAR2: ignore writes to the MMIO control page ───────────────── */
  if (BARs[2]->size() > 0 && BARs[2]->range().contains(pkt->getAddr())) {
    DPRINTF(CxlMemory, "[BWB] BAR2 write at offset 0x%lx (ignored)\n",
            pkt->getAddr() - BARs[2]->addr());
    if (pkt->needsResponse())
      pkt->makeResponse();
    return cxl_mem_latency_;
  }

  /* ── BAR0: data write ───────────────────────────────────────────────
   * 1. access() commits the payload to mapped_cache_ (read-coherent store).
   * 2. bwbWrite() logs the write in the BWB ring buffer + metadata array.
   * 3. ssdWrite() updates the page-cache state and issues SimpleSSD timing.
   * ─────────────────────────────────────────────────────────────── */
  access(pkt);

  /* BWB tracking: convert BAR0 packet address to a 4KB-page LBA, then
   * copy from mapped_cache_ (already written by access()) into the slot. */
  uint64_t ssd_start  = physicalAddrToSSDAddr(pkt->getAddr());
  uint64_t lba        = ssd_start >> BWB_PAGE_BITS;  /* 4KB page number */
  uint8_t *host_addr  = toHostAddr(pkt->getAddr());
  bwbWrite(lba, host_addr, pkt->getSize());

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
  Tick storage_latency = 0; // record the latency for simplessd

  uint64_t ssd_start = physicalAddrToSSDAddr(pkt->getAddr());
#ifndef CXL_SSD_NO_CACHE
  uint64_t logical_frame = ssd_start & (~(logical_page_size_ - 1));

  uint64_t index = evict_strategy->access(logical_frame);

  DPRINTF(CxlMemory, "ssd_read ssd_start: %lx, page_index: %lx\n", ssd_start,
          index);

  auto &page = pages[index];

  if (page.IsValid() && page.CacheHit(ssd_start)) {
    cache_hit_counts_ += 1;
    return latency_;
  }

  if (page.IsDirty()) {
    /* ── Dirty-page writeback before read ────────────────────────────
     * Must evict the dirty occupant to NAND before loading the new page.
     * L_block: pcie_latency_ (2280 ns PCIe DMA) + real SimpleSSD NAND
     * write timing.  Replaces the old hardcoded 35250000 (~35 ms).
     * ─────────────────────────────────────────────────────────────── */
    uint64_t dirty_addr_start = page.tag_;
    uint64_t dirty_page_size  = logical_page_size_;
    uint64_t write_latency    = 0;

    SimpleSSD::HIL::Request write_back_request(&write_latency);
    write_back_request.reqID         = ++instruction_id;
    write_back_request.range.slpn    = dirty_addr_start / logical_page_size_;
    write_back_request.range.nlp     = dirty_page_size  / logical_page_size_;
    write_back_request.offset        = dirty_addr_start % logical_page_size_;
    write_back_request.length        = dirty_page_size;
    write_back_request.function      = [](uint64_t, void *) {};
    write_back_request.context       = (void *)instruction_id;
    pHIL->write(write_back_request);

    /* L_block: PCIe DMA overhead + real NAND write latency */
    storage_latency += pcie_latency_ + write_latency * 10;
    page.ClearDirty();
  }
  page.SetValid();
  page.SetTag(ssd_start);

#endif
  /* ── NAND read to fill the cache slot ────────────────────────────────
   * L_block: pcie_latency_ (2280 ns) + real SimpleSSD NAND read timing.
   * ─────────────────────────────────────────────────────────────── */
  uint64_t read_latency = 0;
  SimpleSSD::HIL::Request request(&read_latency);
  request.reqID         = ++instruction_id;
  request.range.slpn    = ssd_start / logical_page_size_;
  request.range.nlp     = pkt->getSize() / logical_page_size_;
  request.offset        = ssd_start % logical_page_size_;
  request.length        = pkt->getSize();
  request.function      = [](uint64_t, void *) {};
  request.context       = (void *)instruction_id;
  pHIL->read(request);

  /* L_block: PCIe DMA overhead + real NAND read latency */
  storage_latency += pcie_latency_ + read_latency * 10;

  DPRINTF(CxlMemory, "[ssdRead] cache-miss latency %ld ns, simTick %ld\n",
          storage_latency, engine.getCurrentTick());

  /* ── Block-Read Merging (Overlay) ─────────────────────────────────
   * NAND timing has been accounted for above.  Before returning the
   * packet to the host, overlay any dirty BWB cachelines for this LBA.
   * The packet data was populated by access() from mapped_cache_, which
   * reflects the DRAM-side view.  BWB data may be newer for specific
   * cachelines if a byte-path write raced with this block read (e.g. the
   * kernel's page was partially reclaimed between the two operations).
   * We do NOT invalidate the BWB entry here — the data remains live for
   * a future NAND flush via GC or a subsequent block write.
   * ─────────────────────────────────────────────────────────────── */
  {
    uint64_t     lba      = ssd_start >> BWB_PAGE_BITS;
    uint64_t     page_off = ssd_start & (BWB_PAGE_SIZE - 1);
    BwbSlotMeta *meta     = bwbLookup(lba);
    if (meta) {
      uint32_t       slot     = meta->log_index & (BWB_MAX_SLOTS - 1);
      uint8_t       *pkt_data = pkt->getPtr<uint8_t>();
      const uint8_t *bwb_page = bwb_data_ + (uint64_t)slot * BWB_PAGE_SIZE;
      uint64_t       bm       = meta->dirty_bitmap;

      while (bm) {
        int      cl         = __builtin_ctzll(bm);
        uint64_t cl_start   = static_cast<uint64_t>(cl) * BWB_CL_SIZE;
        uint64_t cl_end     = cl_start + BWB_CL_SIZE;
        /* Copy only the portion of this cacheline that the packet covers. */
        if (cl_end > page_off && cl_start < page_off + pkt->getSize()) {
          uint64_t copy_start = std::max(cl_start, page_off);
          uint64_t copy_end   = std::min(cl_end, page_off + pkt->getSize());
          std::memcpy(pkt_data  + (copy_start - page_off),
                      bwb_page  +  copy_start,
                      copy_end  -  copy_start);
        }
        bm &= bm - 1;  /* clear lowest set bit */
      }

      DPRINTF(CxlMemory,
              "[BWB-MERGE-R] Overlaid BWB slot=%u lba=0x%lx page_off=0x%lx "
              "pkt_size=%u dirty_bitmap=0x%016lx\n",
              slot, lba, page_off, pkt->getSize(), meta->dirty_bitmap);
    }
  }

  return storage_latency + latency_;
}

Tick CxlMemory::ssdWrite(PacketPtr pkt) {
  if (!ssdAddrCheck(pkt)) {
    assert(0);
  }
  Tick storage_latency = 0;

  uint64_t ssd_start = physicalAddrToSSDAddr(pkt->getAddr());

  /* ── Block-Write Merging (RAW Consistency) ────────────────────────
   * The kernel routed this write via the block path (NVMe).  Before the
   * data reaches the NAND timing model we check whether the BWB holds
   * any dirty cachelines for the same 4KB page.  If so, those cachelines
   * represent byte-path writes that have been acknowledged to the host but
   * not yet flushed to NAND — they must be overlaid onto mapped_cache_ so
   * that the NAND sees a coherent merged page.  The BWB entry is then
   * invalidated immediately to prevent a duplicate GC flush later.
   * ─────────────────────────────────────────────────────────────── */
  {
    uint64_t      lba  = ssd_start >> BWB_PAGE_BITS;
    BwbSlotMeta  *meta = bwbLookup(lba);
    if (meta) {
      uint32_t  slot     = meta->log_index & (BWB_MAX_SLOTS - 1);
      uint8_t  *dst      = reinterpret_cast<uint8_t *>(mapped_cache_)
                           + lba * BWB_PAGE_SIZE;
      bwbOverlayCachelines(dst, slot, meta->dirty_bitmap);
      bwbInvalidate(lba);
      DPRINTF(CxlMemory,
              "[BWB-MERGE-W] Overlaid BWB slot=%u lba=0x%lx "
              "dirty_bitmap=0x%016lx before NAND write\n",
              slot, lba, meta->dirty_bitmap);
    }
  }

#ifndef CXL_SSD_NO_CACHE
  uint64_t logical_frame = ssd_start & (~(logical_page_size_ - 1));

  uint64_t index = evict_strategy->access(logical_frame);

  DPRINTF(CxlMemory, "ssd_write ssd_start: %lx, page_index: %lx\n", ssd_start,
          index);

  auto &page = pages[index];
  if (page.IsValid() && page.CacheHit(ssd_start)) {
    cache_hit_counts_ += 1;
    return latency_;
  }

  if (page.IsDirty()) {
    /* ── Dirty-page writeback (eviction) ─────────────────────────────
     * A dirty cache line must be flushed to NAND before we can reuse the
     * slot.  This is a NAND block operation: add pcie_latency_ (2280 ns)
     * to model the PCIe DMA setup overhead, then add the real SimpleSSD
     * NAND write timing.  The old hardcoded 35250000 (~35 ms) is removed.
     * ─────────────────────────────────────────────────────────────── */
    uint64_t dirty_addr_start = page.tag_;
    uint64_t dirty_page_size  = logical_page_size_;
    uint64_t write_latency    = 0;

    SimpleSSD::HIL::Request write_back_request(&write_latency);
    write_back_request.reqID         = ++instruction_id;
    write_back_request.range.slpn    = dirty_addr_start / logical_page_size_;
    write_back_request.range.nlp     = dirty_page_size  / logical_page_size_;
    write_back_request.offset        = dirty_addr_start % logical_page_size_;
    write_back_request.length        = dirty_page_size;
    write_back_request.function      = [](uint64_t, void *) {};
    write_back_request.context       = (void *)instruction_id;
    pHIL->write(write_back_request);

    /* L_block component: PCIe DMA overhead + real NAND write latency */
    storage_latency += pcie_latency_ + write_latency * 10;
    page.ClearDirty();
  }

  page.SetValid();
  page.SetTag(ssd_start);
  page.SetDirty();

  /* ── RMW read-side: load existing NAND page into cache ──────────────
   * NVMe/NAND block read.  Add pcie_latency_ for the DMA overhead.
   * ─────────────────────────────────────────────────────────────── */
  uint64_t read_latency = 0;
  SimpleSSD::HIL::Request request(&read_latency);
  request.reqID         = ++instruction_id;
  request.range.slpn    = ssd_start / logical_page_size_;
  request.range.nlp     = pkt->getSize() / logical_page_size_;
  request.offset        = ssd_start % logical_page_size_;
  request.length        = pkt->getSize();
  request.function      = [](uint64_t, void *) {};
  request.context       = (void *)instruction_id;
  pHIL->read(request);

  /* L_block component: PCIe DMA overhead + real NAND read latency */
  storage_latency += pcie_latency_ + read_latency * 10;

  DPRINTF(CxlMemory, "[ssdWrite] cache-miss latency %ld ns, simTick %ld\n",
          storage_latency, engine.getCurrentTick());
#else  // CXL_SSD_NO_CACHE: bypass the page cache, write directly to NAND

  uint64_t write_latency = 0;
  SimpleSSD::HIL::Request write_back_request(&write_latency);
  write_back_request.reqID         = ++instruction_id;
  write_back_request.range.slpn    = ssd_start / logical_page_size_;
  write_back_request.range.nlp     = 1;
  write_back_request.offset        = ssd_start % logical_page_size_;
  write_back_request.length        = logical_page_size_;
  write_back_request.function      = [](uint64_t, void *) {};
  write_back_request.context       = (void *)instruction_id;
  pHIL->write(write_back_request);

  /* L_block: PCIe DMA overhead + real NAND write latency */
  storage_latency += pcie_latency_ + write_latency * 10;
#endif // CXL_SSD_NO_CACHE
  return storage_latency + latency_;
}

/* ====================================================================
 * BWB Implementation
 * ==================================================================== */

/**
 * bwbFillSlots - number of occupied ring buffer slots.
 *
 * Uses unsigned subtraction so it wraps correctly when Log_Tail has
 * rolled over 2^32.  The result is clamped to BWB_MAX_SLOTS to handle
 * the edge case where a very slow GC falls more than one full ring
 * behind (shouldn't happen in practice, but safe either way).
 */
uint32_t CxlMemory::bwbFillSlots() const {
  uint32_t tail = log_tail_.load(std::memory_order_relaxed);
  uint32_t head = log_head_.load(std::memory_order_relaxed);
  uint32_t diff = tail - head;  /* unsigned wrap gives correct delta */
  return diff < BWB_MAX_SLOTS ? diff : BWB_MAX_SLOTS;
}

/**
 * bwbWrite - allocate the next ring buffer slot and record metadata.
 *
 * @lba          4KB-page logical block address (ssd_offset >> 12)
 * @data         pointer into mapped_cache_ where access() stored the data
 * @size         payload size in bytes (clamped to BWB_PAGE_SIZE)
 * @dirty_bitmap 64-bit sub-page dirty cacheline bitmap from page_ext
 *
 * Thread-safety: log_tail_ is advanced with fetch_add(acq_rel) so
 * concurrent writers get distinct slots.  The metadata write to a
 * specific set/way is not additionally locked; in the single-CPU gem5
 * simulation that is fine.  Multi-core future work should add a per-set
 * spinlock or use CAS on the way entry.
 */
void CxlMemory::bwbWrite(uint64_t lba, const uint8_t *data, uint64_t size,
                          uint64_t dirty_bitmap) {
  /* Step 1 — Allocate next slot via atomic fetch-and-add on Log_Tail.
   *           Physical_Slot = Log_Index & (BWB_MAX_SLOTS - 1)  [O(1)] */
  uint32_t log_idx = log_tail_.fetch_add(1, std::memory_order_acq_rel);
  uint32_t slot    = log_idx & (BWB_MAX_SLOTS - 1);

  /* Step 2 — Copy payload into the ring buffer data region and record the
   *           reverse LBA map for O(1) GC head→LBA resolution. */
  uint64_t copy_size  = std::min(size, (uint64_t)BWB_PAGE_SIZE);
  uint64_t bwb_offset = (uint64_t)slot * BWB_PAGE_SIZE;
  std::memcpy(bwb_data_ + bwb_offset, data, copy_size);
  bwb_slot_lba_[slot] = lba;  /* reverse map: physical slot → LBA */

  /* Step 3 — Update the set-associative metadata.
   *
   * Set index: lba % BWB_META_SETS (deterministic, O(1)).
   * Way selection: prefer an empty way; if all are occupied, evict the
   * entry with the lowest (oldest) log_index.  This maintains temporal
   * ordering: the newest metadata for a given LBA always survives,
   * which is required for correct I/O merging (Sprint 4).             */
  uint32_t   set_idx  = (uint32_t)(lba % BWB_META_SETS);
  BwbMetaSet &mset    = bwb_meta_[set_idx];

  int      target_way  = -1;
  uint32_t oldest_log  = UINT32_MAX;
  int      oldest_way  = 0;

  for (int w = 0; w < (int)BWB_META_WAYS; ++w) {
    if (!mset.ways[w].valid) {
      target_way = w;
      break;
    }
    if (mset.ways[w].log_index < oldest_log) {
      oldest_log = mset.ways[w].log_index;
      oldest_way = w;
    }
  }
  if (target_way == -1)
    target_way = oldest_way;  /* evict oldest entry */

  mset.ways[target_way] = BwbSlotMeta{
    .lba          = lba,
    .dirty_bitmap = dirty_bitmap,
    .log_index    = log_idx,
    .valid        = true,
  };

  DPRINTF(CxlMemory,
    "[BWB] Write slot=%u log_idx=%u lba=0x%lx set=%u way=%u "
    "fill=%u/%u dirty_bitmap=0x%016lx\n",
    slot, log_idx, lba, set_idx, target_way,
    bwbFillSlots(), BWB_MAX_SLOTS, dirty_bitmap);
}

/**
 * bwbLookup - find the most-recent valid metadata entry for @p lba.
 * Returns nullptr if the LBA is not currently in the BWB.
 * O(BWB_META_WAYS) = O(1).
 */
BwbSlotMeta *CxlMemory::bwbLookup(uint64_t lba) {
  uint32_t   set_idx = (uint32_t)(lba % BWB_META_SETS);
  BwbMetaSet &mset   = bwb_meta_[set_idx];

  /* Among all valid entries for this LBA, return the one with the
   * highest (most recent) log_index. */
  BwbSlotMeta *result     = nullptr;
  uint32_t     best_log   = 0;

  for (int w = 0; w < (int)BWB_META_WAYS; ++w) {
    if (mset.ways[w].valid && mset.ways[w].lba == lba) {
      if (!result || mset.ways[w].log_index > best_log) {
        result   = &mset.ways[w];
        best_log = mset.ways[w].log_index;
      }
    }
  }
  return result;
}

/**
 * bwbInvalidate - logically remove all metadata entries for @p lba.
 * Sets valid=false on every matching way.  O(1) because BWB_META_WAYS=4.
 * The ring buffer data slot is NOT zeroed; it will be silently reused
 * when Log_Tail wraps around to the same physical slot.
 */
void CxlMemory::bwbInvalidate(uint64_t lba) {
  uint32_t   set_idx = (uint32_t)(lba % BWB_META_SETS);
  BwbMetaSet &mset   = bwb_meta_[set_idx];

  for (int w = 0; w < (int)BWB_META_WAYS; ++w) {
    if (mset.ways[w].valid && mset.ways[w].lba == lba)
      mset.ways[w].invalidate();  /* O(1) logical invalidation */
  }

  DPRINTF(CxlMemory, "[BWB] Invalidated metadata for lba=0x%lx set=%u\n",
          lba, set_idx);
}

/**
 * handleBar2Read - respond to a host OS read of the BWB status registers.
 *
 * The host uses readl() (32-bit) or readq() (64-bit) to poll the fill
 * level.  We decode the offset within the 4 KiB BAR2 window, populate
 * the packet with the current value, and return the CXL.mem latency.
 */
Tick CxlMemory::handleBar2Read(PacketPtr pkt) {
  Addr     offset   = pkt->getAddr() - BARs[2]->addr();
  uint32_t fill_s   = bwbFillSlots();
  uint64_t fill_b   = (uint64_t)fill_s * BWB_PAGE_SIZE;
  uint64_t val64    = 0;

  switch (offset) {
    case BWB_REG_LOG_TAIL:    val64 = log_tail_.load(std::memory_order_relaxed); break;
    case BWB_REG_LOG_HEAD:    val64 = log_head_.load(std::memory_order_relaxed); break;
    case BWB_REG_FILL_SLOTS:  val64 = fill_s;         break;
    case BWB_REG_FILL_BYTES:  val64 = fill_b;         break;
    case BWB_REG_CAP_SLOTS:   val64 = BWB_MAX_SLOTS;  break;
    case BWB_REG_CAP_BYTES:   val64 = BWB_CAPACITY;   break;
    default:
      DPRINTF(CxlMemory, "[BWB] BAR2 read unknown offset 0x%lx\n", offset);
      val64 = 0;
      break;
  }

  /* Packet data is in little-endian format to match x86 host byte order. */
  if (pkt->getSize() == 4) {
    pkt->setLE<uint32_t>(static_cast<uint32_t>(val64));
  } else if (pkt->getSize() == 8) {
    pkt->setLE<uint64_t>(val64);
  } else {
    std::memset(pkt->getPtr<uint8_t>(), 0, pkt->getSize());
  }

  if (pkt->needsResponse())
    pkt->makeResponse();

  DPRINTF(CxlMemory,
    "[BWB] BAR2 read offset=0x%lx size=%u val=0x%016lx "
    "(tail=%u head=%u fill=%u slots / %lu MB)\n",
    offset, pkt->getSize(), val64,
    log_tail_.load(std::memory_order_relaxed),
    log_head_.load(std::memory_order_relaxed),
    fill_s, fill_b >> 20);

  return cxl_mem_latency_;
}

/* ====================================================================
 * BWB Background GC
 * ==================================================================== */

/**
 * scheduleGcPoll - reschedule the GC wakeup event.
 *
 * Called from startup() (first fire) and at the tail of every runGcStep()
 * invocation so the poller keeps ticking for the lifetime of the simulation.
 */
void CxlMemory::scheduleGcPoll() {
  schedule(&gcEvent_, curTick() + BWB_GC_POLL_TICKS);
}

/**
 * runGcStep - one GC wakeup.
 *
 * Watermark logic:
 *   > HWM (85%): activate GC and drain slots.
 *   < LWM (70%): deactivate GC (background drain stops).
 *   Between HWM and LWM: keep draining if already active (hysteresis band).
 *
 * Per-slot drain:
 *   1. Read LBA from bwb_slot_lba_[physical_slot].            [O(1)]
 *   2. Search bwb_meta_[set] for an entry matching log_index. [O(4)]
 *   3a. Match found (live):  issue pHIL->write(), advance head, invalidate.
 *   3b. No match (stale):    advance head silently (data overwritten by
 *                            a newer bwbWrite() for the same LBA).
 */
void CxlMemory::runGcStep() {
  uint32_t fill = bwbFillSlots();

  /* Watermark hysteresis */
  if (!gcActive_ && fill > BWB_GC_HIGH_WM) {
    gcActive_ = true;
    DPRINTF(CxlMemory,
            "[BWB-GC] TRIGGERED: fill=%u > HWM=%u  (%.1f%%)\n",
            fill, BWB_GC_HIGH_WM, 100.0 * fill / BWB_MAX_SLOTS);
  } else if (gcActive_ && fill < BWB_GC_LOW_WM) {
    gcActive_ = false;
    DPRINTF(CxlMemory,
            "[BWB-GC] PAUSED:    fill=%u < LWM=%u  (%.1f%%)\n",
            fill, BWB_GC_LOW_WM, 100.0 * fill / BWB_MAX_SLOTS);
  }

  if (gcActive_) {
    uint32_t drained = 0;

    while (drained < BWB_GC_BATCH && bwbFillSlots() > BWB_GC_LOW_WM) {
      uint32_t head = log_head_.load(std::memory_order_acquire);
      uint32_t slot = head & (BWB_MAX_SLOTS - 1);
      uint64_t lba  = bwb_slot_lba_[slot];

      /* ── Validate: find the metadata entry whose log_index == head ──
       * We search the set for this LBA directly rather than using the
       * high-water bwbLookup() (which returns the newest entry).  An entry
       * with log_index == head is the authoritative live record for this
       * physical slot.  Any entry with a higher log_index means the LBA
       * was rewritten after this slot was allocated — the current slot's
       * data is stale and can be discarded without a NAND write.          */
      uint32_t    set_idx  = static_cast<uint32_t>(lba % BWB_META_SETS);
      BwbMetaSet &mset     = bwb_meta_[set_idx];
      bool        live     = false;
      int         live_way = -1;

      for (int w = 0; w < static_cast<int>(BWB_META_WAYS); ++w) {
        if (mset.ways[w].valid &&
            mset.ways[w].lba       == lba &&
            mset.ways[w].log_index == head) {
          live     = true;
          live_way = w;
          break;
        }
      }

      if (live) {
        /* Issue NAND flush: data already in mapped_cache_ at lba*4K. */
        uint64_t write_latency = 0;
        SimpleSSD::HIL::Request req(&write_latency);
        req.reqID         = ++instruction_id;
        req.range.slpn    = lba;
        req.range.nlp     = 1;
        req.offset        = 0;
        req.length        = BWB_PAGE_SIZE;
        req.function      = [](uint64_t, void *) {};
        req.context       = reinterpret_cast<void *>(
                              static_cast<uintptr_t>(instruction_id));
        pHIL->write(req);

        mset.ways[live_way].invalidate();

        DPRINTF(CxlMemory,
                "[BWB-GC] FLUSH  slot=%u log_idx=%u lba=0x%lx "
                "nand_latency=%lu ns  fill=%u\n",
                slot, head, lba, write_latency, bwbFillSlots());
      } else {
        DPRINTF(CxlMemory,
                "[BWB-GC] STALE  slot=%u log_idx=%u lba=0x%lx "
                "(superseded — discarded)\n",
                slot, head, lba);
      }

      /* Advance Log_Head regardless of live/stale. */
      log_head_.fetch_add(1, std::memory_order_release);
      ++drained;
    }
  }

  scheduleGcPoll();
}

/* ====================================================================
 * BWB I/O Merging Helper
 * ==================================================================== */

/**
 * bwbOverlayCachelines - apply dirty BWB cachelines onto @p dst.
 *
 * @dst          base of the 4KB destination page (in mapped_cache_ or
 *               a packet buffer)
 * @slot         physical ring-buffer slot holding the source data
 * @dirty_bitmap 64-bit mask; bit N → cacheline N (64 bytes at N×64)
 *
 * Iterates only the set bits, so cost is O(number of dirty cachelines),
 * bounded by 64.  No branches per clean cacheline — the bit-scan loop
 * skips them entirely.
 */
void CxlMemory::bwbOverlayCachelines(uint8_t *dst, uint32_t slot,
                                      uint64_t dirty_bitmap) const {
  const uint8_t *src = bwb_data_ + static_cast<uint64_t>(slot) * BWB_PAGE_SIZE;
  uint64_t        bm = dirty_bitmap;

  while (bm) {
    int      cl       = __builtin_ctzll(bm);          /* index of lowest set bit */
    uint64_t cl_off   = static_cast<uint64_t>(cl) * BWB_CL_SIZE;
    std::memcpy(dst + cl_off, src + cl_off, BWB_CL_SIZE);
    bm &= bm - 1;                                     /* clear lowest set bit    */
  }
}

} // namespace gem5