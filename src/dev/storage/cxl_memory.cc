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
 * CxlSsdStats constructor — initialize all scalars with ADD_STAT.
 */
CxlMemory::CxlSsdStats::CxlSsdStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(hostLogicalBytesWritten, statistics::units::Byte::get(),
               "Host logical bytes written (CLWB byte-path + NVMe block-path combined)"),
      ADD_STAT(nandPhysicalBytesWritten, statistics::units::Byte::get(),
               "Physical bytes written to NAND flash (GC, sync, victim evict, block DMA)"),
      ADD_STAT(sdtDenseBypasses, statistics::units::Count::get(),
               "4KB pages routed to NAND: N > N_BASE or no SDT record (dense/background)"),
      ADD_STAT(sdtSparseAbsorptions, statistics::units::Count::get(),
               "4KB pages absorbed into BWB: N <= N_BASE, NAND write skipped (sparse/foreground)"),
      ADD_STAT(bwbVictimFlushes, statistics::units::Count::get(),
               "LRU victim evictions in bwbAllocate() that triggered a forced NAND write"),
      ADD_STAT(nandRmwCounts, statistics::units::Count::get(),
               "NAND read-modify-write cycles (read-before-write on ssdWrite cache miss)")
{
    using namespace statistics;
    hostLogicalBytesWritten .flags(total | nonan);
    nandPhysicalBytesWritten.flags(total | nonan);
    sdtDenseBypasses        .flags(total | nonan);
    sdtSparseAbsorptions    .flags(total | nonan);
    bwbVictimFlushes        .flags(total | nonan);
    nandRmwCounts           .flags(total | nonan);
}

/**
 * CxlMemory
 */

CxlMemory::CxlMemory(const Param &p)
    : PciDevice(p), latency_(p.latency), cxl_mem_latency_(p.cxl_mem_latency),
      pcie_latency_(p.pcie_latency),
      pHIL(new SimpleSSD::HIL::HIL(ssdConfig)),
      gcEvent_([this]{ runGcStep(); }, name() + ".gcEvent"),
      enableCobraPolicy_(p.enable_cobra_policy),
      stats_(this) {
  data_fd_ = open("./CxlSSD.img", O_RDWR | O_CREAT, 0666);
  if (data_fd_ == -1) {
    perror("Error opening file");
    assert(0);
  }
  // Only extend the file if it is smaller than the required capacity.
  // On a fresh run the file starts at 0 bytes; ftruncate fills with zeros
  // (sparse).  On checkpoint restore the file already holds SSD data and
  // must NOT be truncated — that would destroy the checkpointed NAND state.
  struct stat st;
  if (fstat(data_fd_, &st) == -1 || (uint64_t)st.st_size < CXL_SSD_CAPACITY) {
    if (ftruncate(data_fd_, CXL_SSD_CAPACITY) == -1) {
      perror("Error setting file size");
      assert(0);
    }
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
  /* bwb_meta_ is a fixed-size class member (256-set × 4-way SRAM array).
   * Value-initialised by the {} initialiser in the class definition.    */

  DPRINTF(CxlMemory,
    "[BWB] Initialised: %u slots × %u B = %lu MB ring buffer, "
    "%u-way × %u-set SRAM metadata array (LRU replacement)\n",
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
  /* bwb_meta_ is a class member array — no explicit delete needed. */

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

/* ── Checkpoint serialization ─────────────────────────────────────────────
 *
 * We persist the ring-buffer pointers, GC state, the SRAM metadata array,
 * and the per-slot reverse LBA map.  The 256 MB bwb_data_ ring buffer is
 * NOT checkpointed: it is a write-back cache of mapped_cache_ (CxlSSD.img),
 * which is already durable on disk.  The snoopClwb() accumulation path
 * re-populates individual bwb_data_ slots from mapped_cache_ on the first
 * CLWB after restore, so correctness is preserved without 256 MB of I/O.
 * ──────────────────────────────────────────────────────────────────────── */

void CxlMemory::serialize(CheckpointOut &cp) const {
  PciDevice::serialize(cp);

  uint32_t log_tail = log_tail_.load(std::memory_order_relaxed);
  uint32_t log_head = log_head_.load(std::memory_order_relaxed);
  SERIALIZE_SCALAR(log_tail);
  SERIALIZE_SCALAR(log_head);
  SERIALIZE_SCALAR(gcActive_);
  SERIALIZE_SCALAR(instruction_id);

  SERIALIZE_ARRAY(bwb_slot_lba_, BWB_MAX_SLOTS);

  /* Flatten the 2-D bwb_meta_ array into five parallel 1-D arrays so that
   * gem5's text-format checkpoint can encode each primitive type directly. */
  constexpr uint32_t META_TOTAL = BWB_META_SETS * BWB_META_WAYS;
  uint64_t meta_lba         [META_TOTAL];
  uint64_t meta_dirty_bitmap[META_TOTAL];
  uint32_t meta_log_index   [META_TOTAL];
  uint32_t meta_lru_counter [META_TOTAL];
  uint8_t  meta_valid       [META_TOTAL]; /* bool → uint8_t for safe serialization */

  for (uint32_t s = 0; s < BWB_META_SETS; ++s) {
    for (uint32_t w = 0; w < BWB_META_WAYS; ++w) {
      uint32_t idx = s * BWB_META_WAYS + w;
      const BwbSlotMeta &m = bwb_meta_[s].ways[w];
      meta_lba         [idx] = m.lba;
      meta_dirty_bitmap[idx] = m.dirty_bitmap;
      meta_log_index   [idx] = m.log_index;
      meta_lru_counter [idx] = m.lru_counter;
      meta_valid       [idx] = m.valid ? 1u : 0u;
    }
  }
  SERIALIZE_ARRAY(meta_lba,          META_TOTAL);
  SERIALIZE_ARRAY(meta_dirty_bitmap, META_TOTAL);
  SERIALIZE_ARRAY(meta_log_index,    META_TOTAL);
  SERIALIZE_ARRAY(meta_lru_counter,  META_TOTAL);
  SERIALIZE_ARRAY(meta_valid,        META_TOTAL);
}

void CxlMemory::unserialize(CheckpointIn &cp) {
  PciDevice::unserialize(cp);

  uint32_t log_tail = 0, log_head = 0;
  UNSERIALIZE_SCALAR(log_tail);
  UNSERIALIZE_SCALAR(log_head);
  log_tail_.store(log_tail, std::memory_order_relaxed);
  log_head_.store(log_head, std::memory_order_relaxed);
  UNSERIALIZE_SCALAR(gcActive_);
  UNSERIALIZE_SCALAR(instruction_id);

  UNSERIALIZE_ARRAY(bwb_slot_lba_, BWB_MAX_SLOTS);

  constexpr uint32_t META_TOTAL = BWB_META_SETS * BWB_META_WAYS;
  uint64_t meta_lba         [META_TOTAL];
  uint64_t meta_dirty_bitmap[META_TOTAL];
  uint32_t meta_log_index   [META_TOTAL];
  uint32_t meta_lru_counter [META_TOTAL];
  uint8_t  meta_valid       [META_TOTAL];

  UNSERIALIZE_ARRAY(meta_lba,          META_TOTAL);
  UNSERIALIZE_ARRAY(meta_dirty_bitmap, META_TOTAL);
  UNSERIALIZE_ARRAY(meta_log_index,    META_TOTAL);
  UNSERIALIZE_ARRAY(meta_lru_counter,  META_TOTAL);
  UNSERIALIZE_ARRAY(meta_valid,        META_TOTAL);

  for (uint32_t s = 0; s < BWB_META_SETS; ++s) {
    for (uint32_t w = 0; w < BWB_META_WAYS; ++w) {
      uint32_t idx = s * BWB_META_WAYS + w;
      BwbSlotMeta &m = bwb_meta_[s].ways[w];
      m.lba          = meta_lba         [idx];
      m.dirty_bitmap = meta_dirty_bitmap[idx];
      m.log_index    = meta_log_index   [idx];
      m.lru_counter  = meta_lru_counter [idx];
      m.valid        = (meta_valid[idx] != 0);
    }
  }

  DPRINTF(CxlMemory,
    "[CKPT] Restored: log_tail=%u log_head=%u gcActive=%d instruction_id=%lu\n",
    log_tail, log_head, gcActive_, instruction_id);
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

  /* ── Flush/Sync command: drain entire BWB to NAND ───────────────────
   * fsync() eventually issues a Flush command on the cxl.io path.
   * handleSync() flushes ALL valid BWB/SDT entries — no partial flush.  */
  if (pkt->isFlush()) {
    handleSync();
    if (pkt->needsResponse())
      pkt->makeResponse();
    return cxl_mem_latency_;
  }

  /* ── BAR2: ignore writes to the MMIO control page ───────────────── */
  if (BARs[2]->size() > 0 && BARs[2]->range().contains(pkt->getAddr())) {
    DPRINTF(CxlMemory, "[BWB] BAR2 write at offset 0x%lx (ignored)\n",
            pkt->getAddr() - BARs[2]->addr());
    if (pkt->needsResponse())
      pkt->makeResponse();
    return cxl_mem_latency_;
  }

  /* ── BAR0 cxl.mem byte path: WritebackClean (CLWB from kernel) ─────
   * access() writes the 64-byte cacheline into mapped_cache_.
   * snoopClwb() OR-accumulates the precise cacheline bit in the SDT.
   * Returns cxl.mem latency only — no NAND write, no DMA overhead.
   * When the policy engine is disabled the CLWB data still lands in
   * mapped_cache_ (access() runs) but SDT snooping is skipped.        */
  if (pkt->cmd == MemCmd::WritebackClean) {
    stats_.hostLogicalBytesWritten += pkt->getSize();
    access(pkt);
    if (enableCobraPolicy_)
      snoopClwb(pkt->getAddr(), pkt->getSize());
    Tick cxl_latency = resolve_cxl_mem(pkt);
    DPRINTF(CxlMemory, "[COBRA-SDT] CLWB byte-path addr=0x%lx size=%u "
            "cxl_latency=%ld cobra_policy=%d\n",
            pkt->getAddr(), pkt->getSize(), cxl_latency, enableCobraPolicy_);
    return cxl_latency;
  }

  /* ── BAR0 cxl.io block path: WriteReq (NVMe DMA from block layer) ──
   * 1. access() commits the full 4KB payload to mapped_cache_.
   * 2. bwbWrite() logs the write in the ring buffer + metadata.
   * 3. ssdWrite() applies the Phase 2 policy engine and writes to NAND
   *    only if the SDT indicates dense/background I/O (N > N_BASE).   */
  stats_.hostLogicalBytesWritten += pkt->getSize();
  access(pkt);

  uint64_t ssd_start  = physicalAddrToSSDAddr(pkt->getAddr());
  uint64_t lba        = ssd_start >> BWB_PAGE_BITS;
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

  /* CleanEvict: no data payload, nothing to commit.
   * WritebackClean (CLWB): data IS present — fall through so the 64-byte
   * cacheline is written to mapped_cache_ (BWB backing store).          */
  if (pkt->cmd == MemCmd::CleanEvict) {
    DPRINTF(CxlMemory, "CleanEvict on 0x%x: no data, skipping\n", pkt->getAddr());
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
  Tick storage_latency = 0;

  uint64_t ssd_start = physicalAddrToSSDAddr(pkt->getAddr());
  uint64_t lba       = ssd_start >> BWB_PAGE_BITS;
  uint64_t page_off  = ssd_start & (BWB_PAGE_SIZE - 1);

  /* ── Step 1: BWB (SDT) lookup — check before DRAM cache and NAND ──────
   *
   * The BWB is the most up-to-date store for any LBA that has received
   * CLWB byte-path writes since the last GC flush.  mapped_cache_ is
   * always the authoritative backing store (access() keeps it current),
   * so the packet payload is already correct from the access() call in
   * read().  Here we only decide LATENCY and whether NAND timing applies.
   *
   * Full BWB hit  (all requested cachelines covered by dirty_bitmap):
   *   → Return cxl_mem_latency_ only.  No NAND read needed.
   *
   * Partial / No BWB hit:
   *   → Fall through to DRAM cache / NAND path below for correct timing.
   *   → mapped_cache_ already has the freshest data; NO extra overlay
   *     copy is needed or performed (see Bug Note below).
   *
   * Bug Note: the previous implementation overlaid bwb_data_ onto the
   * packet buffer AFTER access() had already filled it from mapped_cache_.
   * Because snoopClwb() didn't sync bwb_data_ on accumulation, bwb_data_
   * could be stale for cachelines written after the first CLWB to an LBA,
   * causing the overlay to corrupt the correct mapped_cache_ data — the
   * root cause of the observed segmentation fault.  The overlay is now
   * removed; bwb_data_ is kept in sync by snoopClwb() for the write-path
   * merge in ssdWrite() which does still need it.
   * ─────────────────────────────────────────────────────────────── */
  {
    BwbSlotMeta *meta = bwbLookup(lba);
    if (meta) {
      /* Compute the bitmask of cachelines the packet spans. */
      uint64_t cl_first    = page_off / BWB_CL_SIZE;
      uint64_t cl_last     = (page_off + pkt->getSize() - 1) / BWB_CL_SIZE;
      uint64_t needed_bits = 0;
      for (uint64_t cl = cl_first; cl <= cl_last && cl < BWB_CL_PER_PAGE; ++cl)
        needed_bits |= (1ULL << cl);

      if ((meta->dirty_bitmap & needed_bits) == needed_bits) {
        /* Full BWB hit: packet payload already correct from access().
         * Return byte-path latency — no NAND access required.          */
        DPRINTF(CxlMemory,
          "[BWB-READ-HIT] Full hit lba=0x%lx page_off=0x%lx "
          "pkt_size=%u needed=0x%016lx bitmap=0x%016lx\n",
          lba, page_off, pkt->getSize(), needed_bits, meta->dirty_bitmap);
        return cxl_mem_latency_;
      }

      /* Partial hit: some CLs in BWB, rest needs NAND timing.
       * mapped_cache_ is authoritative for ALL CLs (CLWB and block path
       * both update it), so the packet data from access() is already
       * correct.  We only add NAND timing below.                       */
      DPRINTF(CxlMemory,
        "[BWB-READ-PARTIAL] lba=0x%lx needed=0x%016lx bitmap=0x%016lx "
        "-> NAND timing added\n",
        lba, needed_bits, meta->dirty_bitmap);
    }
  }

  /* ── Step 2: DRAM page cache + NAND timing ───────────────────────────
   * mapped_cache_ (populated by access()) already holds the correct data.
   * What follows is purely latency accounting — no additional data copies.
   * ─────────────────────────────────────────────────────────────── */
#ifndef CXL_SSD_NO_CACHE
  uint64_t logical_frame = ssd_start & (~(logical_page_size_ - 1));
  uint64_t index         = evict_strategy->access(logical_frame);

  DPRINTF(CxlMemory, "ssd_read ssd_start: %lx, page_index: %lx\n",
          ssd_start, index);

  auto &page = pages[index];

  if (page.IsValid() && page.CacheHit(ssd_start)) {
    cache_hit_counts_ += 1;
    /* DRAM cache hit: packet already correct from access(); DRAM latency. */
    return latency_;
  }

  if (page.IsDirty()) {
    /* ── Dirty-page writeback before read ──────────────────────────────
     * Must evict the dirty occupant to NAND before loading the new page.
     * ─────────────────────────────────────────────────────────────── */
    uint64_t dirty_addr_start = page.tag_;
    uint64_t dirty_page_size  = logical_page_size_;
    uint64_t write_latency    = 0;

    SimpleSSD::HIL::Request write_back_request(&write_latency);
    write_back_request.reqID      = ++instruction_id;
    write_back_request.range.slpn = dirty_addr_start / logical_page_size_;
    write_back_request.range.nlp  = dirty_page_size  / logical_page_size_;
    write_back_request.offset     = dirty_addr_start % logical_page_size_;
    write_back_request.length     = dirty_page_size;
    write_back_request.function   = [](uint64_t, void *) {};
    write_back_request.context    = (void *)instruction_id;
    pHIL->write(write_back_request);
    stats_.nandPhysicalBytesWritten += BWB_PAGE_SIZE;

    storage_latency += pcie_latency_ + write_latency * 10;
    page.ClearDirty();
  }
  page.SetValid();
  page.SetTag(ssd_start);

#endif
  /* ── NAND read timing (data already in packet from access()) ─────── */
  uint64_t read_latency = 0;
  SimpleSSD::HIL::Request request(&read_latency);
  request.reqID      = ++instruction_id;
  request.range.slpn = ssd_start / logical_page_size_;
  request.range.nlp  = pkt->getSize() / logical_page_size_;
  request.offset     = ssd_start % logical_page_size_;
  request.length     = pkt->getSize();
  request.function   = [](uint64_t, void *) {};
  request.context    = (void *)instruction_id;
  pHIL->read(request);

  storage_latency += pcie_latency_ + read_latency * 10;

  DPRINTF(CxlMemory, "[ssdRead] NAND-miss latency %ld ns, simTick %ld\n",
          storage_latency, engine.getCurrentTick());

  /* NOTE: No BWB overlay here.  mapped_cache_ is the authoritative store;
   * access() already populated the packet from mapped_cache_ before this
   * function was called.  Overlaying bwb_data_ on top would risk writing
   * stale ring-buffer data over the correct packet payload.  The write-path
   * merge in ssdWrite() still overlays bwb_data_ (which snoopClwb() now
   * keeps in sync) to preserve byte-path writes before a block-path flush. */

  return storage_latency + latency_;
}

Tick CxlMemory::ssdWrite(PacketPtr pkt) {
  if (!ssdAddrCheck(pkt)) {
    assert(0);
  }
  Tick storage_latency = 0;

  uint64_t ssd_start = physicalAddrToSSDAddr(pkt->getAddr());

  /* ── COBRA Phase 2: Hardware Policy Engine ────────────────────────
   * Gated by enableCobraPolicy_.  When disabled, all block writes fall
   * through to the standard NAND path (pure baseline mode).
   * ─────────────────────────────────────────────────────────────── */
  if (enableCobraPolicy_) {
    uint64_t     lba  = ssd_start >> BWB_PAGE_BITS;
    BwbSlotMeta *meta = bwbLookup(lba);

    if (meta) {
      int N = __builtin_popcountll(meta->dirty_bitmap);
      if (N <= COBRA_N_BASE) {
        DPRINTF(CxlMemory,
          "[COBRA-POLICY] Sparse lba=0x%lx N=%d <= N_BASE=%d "
          "-> absorbed from BWB, NAND skipped\n", lba, N, COBRA_N_BASE);
        stats_.sdtSparseAbsorptions += 1;
        bwbInvalidate(lba);
        if (pkt->needsResponse()) pkt->makeResponse();
        return cxl_mem_latency_;
      }
      DPRINTF(CxlMemory,
        "[COBRA-POLICY] Dense lba=0x%lx N=%d > N_BASE=%d "
        "-> standard NAND write\n", lba, N, COBRA_N_BASE);
      stats_.sdtDenseBypasses += 1;
      /* Invalidation is handled by the BWB merge section below. */
    } else {
      DPRINTF(CxlMemory,
        "[COBRA-POLICY] No SDT record for lba=0x%lx "
        "(background bypass) -> standard NAND write\n", lba);
      stats_.sdtDenseBypasses += 1;
    }
  }

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
    stats_.nandPhysicalBytesWritten += BWB_PAGE_SIZE; /* dirty-eviction flush */

    /* L_block component: PCIe DMA overhead + real NAND write latency */
    storage_latency += pcie_latency_ + write_latency * 10;
    page.ClearDirty();
  }

  page.SetValid();
  page.SetTag(ssd_start);
  page.SetDirty();

  /* ── RMW read-side: load existing NAND page into cache ──────────────
   * This read + subsequent dirty-eviction write is the RMW cycle.
   * ─────────────────────────────────────────────────────────────── */
  stats_.nandRmwCounts += 1;
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

  stats_.nandPhysicalBytesWritten += BWB_PAGE_SIZE; /* direct no-cache NAND write */
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

  /* Step 3 — Allocate (or evict) a metadata entry via LRU policy.
   * bwbAllocate() finds an invalid way or evicts the LRU victim (flushing
   * it to NAND).  It returns a pointer with lba set and valid=true.     */
  BwbSlotMeta *meta = bwbAllocate(lba);
  meta->dirty_bitmap = dirty_bitmap;
  meta->log_index    = log_idx;

  DPRINTF(CxlMemory,
    "[BWB] Write slot=%u log_idx=%u lba=0x%lx set=%u "
    "fill=%u/%u dirty_bitmap=0x%016lx\n",
    slot, log_idx, lba, (uint32_t)(lba % BWB_META_SETS),
    bwbFillSlots(), BWB_MAX_SLOTS, dirty_bitmap);
}

/**
 * bwbLookup - associative tag search with LRU promotion.
 *
 * Scans all BWB_META_WAYS ways in the addressed set.  On a hit the
 * matching way's lru_counter is set to the maximum value and all other
 * valid ways in the same set are decremented, implementing the hardware
 * "promote-on-access" LRU update in O(Ways) = O(1) time.
 * Returns nullptr on a miss.
 */
BwbSlotMeta *CxlMemory::bwbLookup(uint64_t lba) {
  uint32_t   set_idx = (uint32_t)(lba % BWB_META_SETS);
  BwbMetaSet &mset   = bwb_meta_[set_idx];

  for (int w = 0; w < (int)BWB_META_WAYS; ++w) {
    if (mset.ways[w].valid && mset.ways[w].lba == lba) {
      /* LRU promotion: set this way to max, decrement all others */
      mset.ways[w].lru_counter = BWB_META_WAYS - 1;
      for (int o = 0; o < (int)BWB_META_WAYS; ++o) {
        if (o != w && mset.ways[o].valid && mset.ways[o].lru_counter > 0)
          --mset.ways[o].lru_counter;
      }
      return &mset.ways[w];
    }
  }
  return nullptr;
}

/**
 * bwbAllocate - allocate a metadata entry for @p lba with LRU replacement.
 *
 * Eviction path: if all ways are valid, the way with the lowest
 * lru_counter is the LRU victim.  Its data is flushed to NAND via
 * pHIL->write() before the slot is reused — preventing silent data loss.
 *
 * Returns a pointer to the newly initialised entry with lba set,
 * valid=true, dirty_bitmap=0.  Caller must populate log_index and
 * dirty_bitmap.
 */
BwbSlotMeta *CxlMemory::bwbAllocate(uint64_t lba) {
  uint32_t   set_idx = (uint32_t)(lba % BWB_META_SETS);
  BwbMetaSet &mset   = bwb_meta_[set_idx];

  /* 1. Try an invalid (empty) way first */
  for (int w = 0; w < (int)BWB_META_WAYS; ++w) {
    if (!mset.ways[w].valid) {
      mset.ways[w].invalidate();
      mset.ways[w].lba         = lba;
      mset.ways[w].valid       = true;
      mset.ways[w].lru_counter = BWB_META_WAYS - 1;
      for (int o = 0; o < (int)BWB_META_WAYS; ++o) {
        if (o != w && mset.ways[o].valid && mset.ways[o].lru_counter > 0)
          --mset.ways[o].lru_counter;
      }
      return &mset.ways[w];
    }
  }

  /* 2. All ways occupied — select LRU victim (lowest lru_counter) */
  int victim_way = 0;
  for (int w = 1; w < (int)BWB_META_WAYS; ++w) {
    if (mset.ways[w].lru_counter < mset.ways[victim_way].lru_counter)
      victim_way = w;
  }
  BwbSlotMeta &victim = mset.ways[victim_way];

  /* Flush victim page to NAND before reuse */
  uint64_t write_latency = 0;
  SimpleSSD::HIL::Request req(&write_latency);
  req.reqID      = ++instruction_id;
  req.range.slpn = victim.lba;
  req.range.nlp  = 1;
  req.offset     = 0;
  req.length     = BWB_PAGE_SIZE;
  req.function   = [](uint64_t, void *) {};
  req.context    = reinterpret_cast<void *>(static_cast<uintptr_t>(instruction_id));
  pHIL->write(req);

  stats_.bwbVictimFlushes        += 1;
  stats_.nandPhysicalBytesWritten += BWB_PAGE_SIZE;

  DPRINTF(CxlMemory,
    "[BWB-ALLOC] LRU evict set=%u victim_way=%d lba=0x%lx "
    "dirty_bitmap=0x%016lx nand_latency=%lu ns -> new lba=0x%lx\n",
    set_idx, victim_way, victim.lba, victim.dirty_bitmap,
    write_latency, lba);

  /* Overwrite victim with the new entry */
  victim.lba          = lba;
  victim.dirty_bitmap = 0;
  victim.log_index    = 0;   /* caller sets this */
  victim.lru_counter  = BWB_META_WAYS - 1;
  victim.valid        = true;

  /* Demote all other ways */
  for (int o = 0; o < (int)BWB_META_WAYS; ++o) {
    if (o != victim_way && mset.ways[o].valid && mset.ways[o].lru_counter > 0)
      --mset.ways[o].lru_counter;
  }

  return &victim;
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
        stats_.nandPhysicalBytesWritten += BWB_PAGE_SIZE; /* GC drain flush */

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

/* ====================================================================
 * COBRA Phase 2: SDT Snoop & Sync
 * ==================================================================== */

/**
 * snoopClwb - SDT update for a WritebackClean (CLWB) packet.
 *
 * Called from write() for every cxl.mem byte-path write.  The 64-byte
 * cacheline payload has already been committed to mapped_cache_ by
 * access() before this is called.
 *
 * Algorithm (O(BWB_META_WAYS) = O(1)):
 *   1. Convert BAR0 physical address → SSD offset → LBA + cacheline bit.
 *   2. If an existing BWB entry for this LBA exists, OR in the new bit.
 *      This accumulates all CLWB pushes for the same page without
 *      allocating a new ring slot for each 64-byte cacheline.
 *   3. If no entry exists, allocate a new ring slot seeded from the
 *      current state of mapped_cache_ for that page, with only the
 *      specific cacheline bit(s) set in dirty_bitmap.
 */
void CxlMemory::snoopClwb(Addr paddr, size_t size) {
  uint64_t ssd_off  = physicalAddrToSSDAddr(paddr);
  uint64_t lba      = ssd_off >> BWB_PAGE_BITS;

  /* Compute the cacheline bit range covered by this write. */
  uint64_t page_off  = ssd_off & (BWB_PAGE_SIZE - 1);
  uint64_t cl_first  = page_off / BWB_CL_SIZE;
  uint64_t cl_last   = (page_off + size - 1) / BWB_CL_SIZE;
  uint64_t new_bits  = 0;
  for (uint64_t cl = cl_first; cl <= cl_last && cl < BWB_CL_PER_PAGE; ++cl)
    new_bits |= (1ULL << cl);

  BwbSlotMeta *meta = bwbLookup(lba);
  if (meta) {
    /* Hit — accumulate bits AND sync the new dirty cachelines from
     * mapped_cache_ into bwb_data_.  Without this sync, subsequent CLWBs
     * to the same LBA update mapped_cache_ (via access()) but leave
     * bwb_data_ stale for those cachelines.  The write-path overlay in
     * ssdWrite() would then overwrite newer mapped_cache_ data with the
     * older ring-buffer data, corrupting the write before NAND flush.   */
    meta->dirty_bitmap |= new_bits;
    {
      uint32_t       slot      = meta->log_index & (BWB_MAX_SLOTS - 1);
      uint8_t       *bwb_page  = bwb_data_ + (uint64_t)slot * BWB_PAGE_SIZE;
      const uint8_t *cache_pg  = reinterpret_cast<const uint8_t *>(mapped_cache_)
                                 + lba * BWB_PAGE_SIZE;
      uint64_t bits = new_bits;
      while (bits) {
        int cl = __builtin_ctzll(bits);
        std::memcpy(bwb_page  + (uint64_t)cl * BWB_CL_SIZE,
                    cache_pg  + (uint64_t)cl * BWB_CL_SIZE,
                    BWB_CL_SIZE);
        bits &= bits - 1;
      }
    }
    DPRINTF(CxlMemory,
      "[SDT] Snoop CLWB UPDATE lba=0x%lx cl_first=%lu cl_last=%lu "
      "new_bits=0x%016lx accum=0x%016lx (bwb_data_ synced)\n",
      lba, cl_first, cl_last, new_bits, meta->dirty_bitmap);
  } else {
    /* Miss — allocate a new SRAM entry (evicting LRU victim if needed).
     * Also allocate a ring data slot so GC/sync can find the payload.  */
    uint32_t log_idx = log_tail_.fetch_add(1, std::memory_order_acq_rel);
    uint32_t slot    = log_idx & (BWB_MAX_SLOTS - 1);
    uint8_t *page_base = reinterpret_cast<uint8_t *>(mapped_cache_)
                         + lba * BWB_PAGE_SIZE;
    std::memcpy(bwb_data_ + (uint64_t)slot * BWB_PAGE_SIZE,
                page_base, BWB_PAGE_SIZE);
    bwb_slot_lba_[slot] = lba;

    meta               = bwbAllocate(lba);
    meta->dirty_bitmap = new_bits;
    meta->log_index    = log_idx;

    DPRINTF(CxlMemory,
      "[SDT] Snoop CLWB NEW    lba=0x%lx cl_first=%lu cl_last=%lu "
      "new_bits=0x%016lx slot=%u\n",
      lba, cl_first, cl_last, new_bits, slot);
  }
}

/**
 * handleSync - full BWB flush to NAND triggered by a Flush/fsync command.
 *
 * Iterates the ENTIRE metadata array (all BWB_META_SETS × BWB_META_WAYS
 * entries) and issues a SimpleSSD write for every valid entry.  After
 * all entries are flushed, all valid bits are cleared and Log_Head is
 * advanced to Log_Tail so the ring buffer appears empty.
 *
 * Partial-offset logic is STRICTLY FORBIDDEN — every valid page must be
 * written regardless of how many cachelines are marked dirty.
 */
void CxlMemory::handleSync() {
  uint32_t flushed = 0;

  DPRINTF(CxlMemory,
    "[COBRA-SYNC] handleSync START — flushing all valid BWB entries\n");

  for (uint32_t s = 0; s < BWB_META_SETS; ++s) {
    for (uint32_t w = 0; w < (uint32_t)BWB_META_WAYS; ++w) {
      BwbSlotMeta &m = bwb_meta_[s].ways[w];
      if (!m.valid)
        continue;

      uint64_t write_latency = 0;
      SimpleSSD::HIL::Request req(&write_latency);
      req.reqID         = ++instruction_id;
      req.range.slpn    = m.lba;
      req.range.nlp     = 1;
      req.offset        = 0;
      req.length        = BWB_PAGE_SIZE;
      req.function      = [](uint64_t, void *) {};
      req.context       = reinterpret_cast<void *>(
                            static_cast<uintptr_t>(instruction_id));
      pHIL->write(req);

      stats_.nandPhysicalBytesWritten += BWB_PAGE_SIZE; /* handleSync flush */

      DPRINTF(CxlMemory,
        "[COBRA-SYNC] Flushed lba=0x%lx dirty_bitmap=0x%016lx "
        "nand_latency=%lu ns\n",
        m.lba, m.dirty_bitmap, write_latency);

      m.invalidate();
      ++flushed;
    }
  }

  /* Advance Log_Head to Log_Tail: ring buffer is now logically empty. */
  log_head_.store(log_tail_.load(std::memory_order_relaxed),
                  std::memory_order_relaxed);

  DPRINTF(CxlMemory,
    "[COBRA-SYNC] handleSync DONE — %u pages flushed, SDT cleared\n",
    flushed);
}

} // namespace gem5