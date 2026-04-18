#include "base/addr_range.hh"
#include "base/bitfield.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/CxlMemory.hh"
#include "debug/CxlMemoryCacheHit.hh"
#include "debug/CxlMemoryCoherency.hh"
#include "dev/pci/device.hh"
#include "dev/storage/simplessd/hil/hil.hh"
#include "dev/storage/simplessd/util/simplessd.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "params/CxlMemory.hh"
#include "sim/eventq.hh"
#include "base/statistics.hh"
#include "base/stats/group.hh"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

/* COBRA Phase 2: Sub-page Dirty Tracker */
#define COBRA_N_BASE 16

namespace gem5 {

#define CXL_SSD_PAGE_LEFT_BITS (12)                       // 12 bit
#define CXL_SSD_CAPACITY (1LL << 32)                      // 4G capacity
#define CXL_SSD_CACHE_CAPACITY (1LL << 24)                // 32M capacity
#define CXL_SSD_PAGE_SIZE (1LL << CXL_SSD_PAGE_LEFT_BITS) // 4K capacity
#define CXL_SSD_CACHE_HIT_STAT (1 << 16)                  // 10 0000 counts
// #define CXL_MEMORY_ENABLE 1
// #define CXL_SSD_NO_CACHE 1

typedef uint64_t Tick;
typedef size_t frame_id;

/**
 * eventengine override the simulator of simplessd
 *
 */
class Engine : public SimpleSSD::Simulator {
private:
  uint64_t simTick;
  SimpleSSD::Event counter;
  std::unordered_map<SimpleSSD::Event, SimpleSSD::EventFunction> eventList;
  std::list<std::pair<SimpleSSD::Event, uint64_t>> eventQueue;

  uint64_t eventHandled;

  bool insertEvent(SimpleSSD::Event, uint64_t, uint64_t * = nullptr);
  bool removeEvent(SimpleSSD::Event);
  bool isEventExist(SimpleSSD::Event, uint64_t * = nullptr);

public:
  Engine();
  ~Engine();

  uint64_t getCurrentTick() override;

  SimpleSSD::Event allocateEvent(SimpleSSD::EventFunction) override;
  void scheduleEvent(SimpleSSD::Event, uint64_t) override;
  void descheduleEvent(SimpleSSD::Event) override;
  bool isScheduled(SimpleSSD::Event, uint64_t * = nullptr) override;
  void deallocateEvent(SimpleSSD::Event) override;
};

struct Page {
  uint64_t tag_;
  uint8_t flag_;

  static const uint64_t valid_offset = 0x0;
  static const uint64_t dirty_offset = 0x1;

  bool IsValid() { return gem5::bits(flag_, valid_offset); }
  bool IsDirty() { return gem5::bits(flag_, dirty_offset); }

  void SetValid() { gem5::set_bit(flag_, valid_offset); }
  void SetDirty() { gem5::set_bit(flag_, dirty_offset); }

  void ClearValid() { gem5::clear_bit(flag_, valid_offset); }
  void ClearDirty() { gem5::clear_bit(flag_, dirty_offset); }

  void SetTag(uint64_t tag) { tag_ = tag & (~(CXL_SSD_PAGE_SIZE - 1)); }

  bool CacheHit(Addr addr) {
    return ((addr >> CXL_SSD_PAGE_LEFT_BITS) ==
            (tag_ >> CXL_SSD_PAGE_LEFT_BITS));
  }
};

extern Engine engine;
extern SimpleSSD::ConfigReader ssdConfig;

enum class EvictStrategyMode { Direct, LRU, FIFO, TwoQ, LFRU };

class LRUCache {
  struct Node {
    // frame means aligned to 4k
    uint64_t logical_frame;
    uint64_t frame_id;
  };

public:
  std::unordered_map<int, std::list<Node>::iterator> mem_;
  std::list<Node> table_;
  size_t capacity_;

  uint64_t allocated_id{0};
  uint64_t total_pages{0};

  // 4k multiples
  LRUCache(size_t capacity) : capacity_(capacity) {
    total_pages = capacity_ / CXL_SSD_PAGE_SIZE;
  }

  uint64_t access(uint64_t key) {
    if (auto it = mem_.find(key); it != mem_.end()) {
      auto frame_id = it->second->frame_id;
      table_.splice(table_.end(), table_, it->second);
      return frame_id;
    }
    if (table_.size() < total_pages) {
      uint64_t value = allocated_id++;
      table_.push_back({key, value});
      mem_.insert({key, --table_.end()});
      return value;
    }
    auto [delete_logical, frame_id] = table_.front();
    mem_.erase(delete_logical);
    table_.erase(table_.begin());
    table_.push_back({key, frame_id});
    mem_.insert({key, --table_.end()});
    return frame_id;
  }
};

class FIFOCache {
public:
  struct Node {
    // frame means aligned to 4k
    uint64_t logical_frame;
    uint64_t frame_id;
  };

public:
  std::unordered_map<uint64_t, std::list<Node>::iterator> mem_;
  std::list<Node> table_;
  size_t capacity_;

  uint64_t allocated_id{0};
  uint64_t total_pages{0};

  // 4k multiples
  FIFOCache(size_t capacity) : capacity_(capacity) {
    total_pages = capacity_ / CXL_SSD_PAGE_SIZE;
  }

  bool Exist(uint64_t key) { return mem_.find(key) != mem_.end(); }
  // FIFO strategy
  uint64_t access(uint64_t key) {
    if (auto it = mem_.find(key); it != mem_.end()) {
      return it->second->frame_id;
    }
    if (table_.size() < total_pages) {
      uint64_t value = allocated_id++;
      table_.push_back({key, value});
      mem_.insert({key, --table_.end()});
      return value;
    }
    auto [delete_logical, frame_id] = table_.front();
    mem_.erase(delete_logical);
    table_.erase(table_.begin());
    table_.push_back({key, frame_id});
    mem_.insert({key, --table_.end()});
    return frame_id;
  }
};

class LFRUCache {
private:
  struct LFRUNode {
    int cnt;  // frequency for lfu
    int time; // for lru
    uint64_t key;
    uint64_t value;
    LFRUNode(int _cnt, int _time, int _key, int _value)
        : cnt(_cnt), time(_time), key(_key), value(_value) {}

    bool operator<(const LFRUNode item) const {
      return cnt == item.cnt ? time < item.time : cnt < item.cnt;
    }
  };

  uint64_t capacity;
  uint64_t time_;
  std::unordered_map<uint64_t, std::set<LFRUNode>::iterator> map_;
  std::set<LFRUNode> mem_;

  uint64_t total_pages_;
  uint64_t allocated_id_{0};

public:
  LFRUCache(int _capacity) : capacity(_capacity), time_(0) {
    total_pages_ = capacity / CXL_SSD_PAGE_SIZE;
  }

  uint64_t access(uint64_t key) {
    if (auto it = map_.find(key); it != map_.end()) {
      auto iterator = it->second;
      auto node = mem_.extract(iterator);
      assert(bool(node));
      node.value().cnt += 1;
      node.value().time = ++time_;
      auto res_value = node.value().value;
      mem_.insert(std::move(node));
      return res_value;
    }
    if (mem_.size() < total_pages_) {
      uint64_t value = allocated_id_++;
      auto insert_pair = mem_.insert(LFRUNode(1, ++time_, key, value));
      assert(insert_pair.second);
      map_.insert({key, insert_pair.first});
      return value;
    }
    assert(mem_.size() == total_pages_);
    auto delete_node = mem_.extract(mem_.begin());
    assert(bool(delete_node));
    map_.erase(delete_node.value().key);
    auto value = delete_node.value().value;
    delete_node.value().cnt = 1;
    delete_node.value().time = ++time_;
    delete_node.value().key = key;
    auto insert_pair = mem_.insert(std::move(delete_node));
    assert(insert_pair.inserted);
    map_.insert({key, insert_pair.position});
    return value;
  }
};

class EvictStrategy {
public:
  virtual uint64_t access(uint64_t logical_frame) = 0;
  virtual ~EvictStrategy() = default;
};

class DirectEvictStrategy : public EvictStrategy {
public:
  uint64_t page_counts;
  DirectEvictStrategy(uint64_t page_counts) : page_counts(page_counts) {}
  uint64_t access(uint64_t logical_frame) override {
    uint64_t tag = logical_frame / CXL_SSD_PAGE_SIZE;
    uint64_t index = tag & (page_counts - 1);
    return index;
  }
};

class LRUEvictStrategy : public EvictStrategy {
public:
  LRUCache cache;
  LRUEvictStrategy(uint64_t capacity) : cache(capacity) {}
  uint64_t access(uint64_t logical_frame) override {
    return cache.access(logical_frame);
  }
};
class FIFOEvictStrategy : public EvictStrategy {
public:
  FIFOCache cache;
  FIFOEvictStrategy(uint64_t capacity) : cache(capacity) {}
  uint64_t access(uint64_t logical_frame) override {
    return cache.access(logical_frame);
  }
};

class LFRUEviceStrategy : public EvictStrategy {
public:
  LFRUCache cache;
  LFRUEviceStrategy(uint64_t capacity) : cache(capacity) {}
  uint64_t access(uint64_t logical_frame) override {
    return cache.access(logical_frame);
  }
};

class TwoQEvictStrategy : public EvictStrategy {
public:
  struct Node {
    // frame means aligned to 4k
    uint64_t logical_frame;
    uint64_t frame_id;
  };

public:
  std::unordered_map<uint64_t, std::list<Node>::iterator> fifo_map_;
  std::list<Node> fifo_list_;

  std::unordered_map<uint64_t, std::list<Node>::iterator> lru_map_;
  std::list<Node> lru_list_;

  size_t capacity_;
  size_t queue_pages_;

  uint64_t allocated_id_{0};
  uint64_t total_pages_{0};

  std::list<uint64_t> free_frame_id_;

  explicit TwoQEvictStrategy(size_t capacity) : capacity_(capacity) {
    total_pages_ = capacity_ / CXL_SSD_PAGE_SIZE;
    queue_pages_ = total_pages_ / 2;
  }

  uint64_t fifoQueueAccess(uint64_t key) {
    if (auto it = fifo_map_.find(key); it != fifo_map_.end()) {
      return it->second->frame_id;
    }

    if (fifo_list_.size() == queue_pages_) {
      auto [delete_logical, frame_id] = fifo_list_.front();
      fifo_map_.erase(delete_logical);
      fifo_list_.erase(fifo_list_.begin());
      fifo_list_.push_back({key, frame_id});
      fifo_map_.insert({key, --fifo_list_.end()});
      return frame_id;
    }
    assert(fifo_list_.size() < queue_pages_);
    uint64_t value = -1;
    if (!free_frame_id_.empty()) {
      value = free_frame_id_.back();
      free_frame_id_.pop_back();
    } else {
      value = allocated_id_++;
    }
    fifo_list_.push_back({key, value});
    fifo_map_.insert({key, --fifo_list_.end()});
    return value;
  }

  uint64_t lruQueueAccess(uint64_t key) {
    if (auto it = lru_map_.find(key); it != lru_map_.end()) {
      auto frame_id = it->second->frame_id;
      lru_list_.splice(lru_list_.end(), lru_list_, it->second);
      return frame_id;
    }
    if (lru_list_.size() == queue_pages_) {
      auto [delete_logical, frame_id] = lru_list_.front();
      lru_map_.erase(delete_logical);
      lru_list_.erase(lru_list_.begin());
      if (!free_frame_id_.empty()) {
        auto last_frame_id = free_frame_id_.back();
        free_frame_id_.pop_back();
        free_frame_id_.push_back(frame_id);
        lru_list_.push_back({key, last_frame_id});
        lru_map_.insert({key, --lru_list_.end()});
        return last_frame_id;
      } else {
        lru_list_.push_back({key, frame_id});
        lru_map_.insert({key, --lru_list_.end()});
        return frame_id;
      }
    }
    assert(lru_list_.size() < queue_pages_);
    uint64_t value = -1;
    if (!free_frame_id_.empty()) {
      value = free_frame_id_.back();
      free_frame_id_.pop_back();
    } else {
      value = allocated_id_++;
    }
    lru_list_.push_back({key, value});
    lru_map_.insert({key, --lru_list_.end()});
    return value;
  }

  bool fifoExist(uint64_t key) {
    return fifo_map_.find(key) != fifo_map_.end();
  }

  bool lruExist(uint64_t key) { return lru_map_.find(key) != lru_map_.end(); }

  uint64_t access(uint64_t logical_frame) override {
    if (lruExist(logical_frame)) {
      return lruQueueAccess(logical_frame);
    }
    if (auto it = fifo_map_.find(logical_frame); it != fifo_map_.end()) {
      auto [delete_logical, frame_id] = *it->second;
      assert(logical_frame == delete_logical);
      fifo_map_.erase(delete_logical);
      fifo_list_.erase(it->second);
      free_frame_id_.push_back(frame_id);
      return lruQueueAccess(logical_frame);
    }
    return fifoQueueAccess(logical_frame);
  }
};

EvictStrategy *Worker(EvictStrategyMode mode, uint64_t capacity);

/* ====================================================================
 * COBRA Byte-Write Buffer (BWB) — gem5 Hardware Model
 *
 * Ring buffer geometry:
 *   65,536 slots × 4 KB/slot = 256 MB  (must be a power of two)
 *   Physical_Slot = Log_Index & (BWB_MAX_SLOTS - 1)   [O(1)]
 *   Log_Tail advances via std::atomic::fetch_add()    [lock-free]
 *   Log_Head advances during GC/drain (Sprint 4+)
 *
 * Set-associative Metadata Array:
 *   4-way, 256 sets  (fixed SRAM budget; 1024 simultaneous LBAs)
 *   Set index = lba_page % BWB_META_SETS
 *   O(1) lookup and O(1) invalidation per set
 *
 * BAR2 MMIO Register Map (4 KiB page, host reads via readq/readl):
 *   0x00  BWB_REG_LOG_TAIL    uint32  RO  monotonically increasing tail
 *   0x04  BWB_REG_LOG_HEAD    uint32  RO  GC head (always 0 until Sprint 4)
 *   0x08  BWB_REG_FILL_SLOTS  uint32  RO  occupied slot count
 *   0x10  BWB_REG_FILL_BYTES  uint64  RO  occupied bytes (fill_slots × 4096)
 *   0x18  BWB_REG_CAP_SLOTS   uint32  RO  total capacity in slots (65536)
 *   0x20  BWB_REG_CAP_BYTES   uint64  RO  total capacity in bytes  (256 MB)
 * ==================================================================== */

static constexpr uint32_t BWB_PAGE_BITS   = 12;                        /* log2(4 KB) */
static constexpr uint32_t BWB_PAGE_SIZE   = 1u << BWB_PAGE_BITS;       /* 4 096 B    */
static constexpr uint32_t BWB_MAX_SLOTS   = 65536u;                    /* must be 2^N */
static constexpr uint64_t BWB_CAPACITY    = (uint64_t)BWB_MAX_SLOTS * BWB_PAGE_SIZE; /* 256 MB */
static constexpr uint32_t BWB_META_WAYS   = 4u;
static constexpr uint32_t BWB_META_SETS   = 256u;                          /* fixed SRAM budget */

/* BAR2 register offsets */
static constexpr uint64_t BWB_REG_LOG_TAIL   = 0x00;
static constexpr uint64_t BWB_REG_LOG_HEAD   = 0x04;
static constexpr uint64_t BWB_REG_FILL_SLOTS = 0x08;
static constexpr uint64_t BWB_REG_FILL_BYTES = 0x10;
static constexpr uint64_t BWB_REG_CAP_SLOTS  = 0x18;
static constexpr uint64_t BWB_REG_CAP_BYTES  = 0x20;

/* Background GC watermarks and timing */
static constexpr uint32_t BWB_GC_HIGH_WM     = BWB_MAX_SLOTS * 85 / 100; /* 55,705 slots */
static constexpr uint32_t BWB_GC_LOW_WM      = BWB_MAX_SLOTS * 70 / 100; /* 45,875 slots */
static constexpr Tick     BWB_GC_POLL_TICKS  = 10000000; /* 10 μs @ 1 tick=1 ps (gem5 default) */
static constexpr uint32_t BWB_GC_BATCH       = 64;      /* max slots drained per wakeup   */

/* Cacheline geometry */
static constexpr uint32_t BWB_CL_SIZE        = 64;                        /* bytes/cacheline */
static constexpr uint32_t BWB_CL_PER_PAGE    = BWB_PAGE_SIZE / BWB_CL_SIZE; /* 64 cls/page  */

/**
 * Per-slot metadata entry in the BWB.
 *
 * Tracks the LBA (in 4KB pages), validity, the sub-page dirty bitmap
 * from the kernel's page_ext (64 cacheline bits per 4KB page), and
 * the Log_Index used to locate the corresponding data in the ring buffer.
 */
struct BwbSlotMeta {
  uint64_t lba;           /**< 4KB-page-aligned logical block address          */
  uint64_t dirty_bitmap;  /**< cacheline dirty bits (SDT record)               */
  uint32_t log_index;     /**< Log_Index that allocated this physical slot      */
  uint32_t lru_counter;   /**< hardware LRU state: higher = more recently used  */
  bool     valid;         /**< slot is occupied and not yet drained by GC      */

  void invalidate() {
    valid        = false;
    lba          = 0;
    dirty_bitmap = 0;
    lru_counter  = 0;
  }
};

/**
 * One set of the 4-way set-associative Metadata Array.
 *
 * On a set miss with all ways valid, the entry with the lowest
 * log_index is evicted (oldest-first policy).  This keeps the metadata
 * consistent with the ring buffer's FIFO ordering.
 */
struct BwbMetaSet {
  BwbSlotMeta ways[BWB_META_WAYS];
};

class CxlMemory : public PciDevice {
private:
  AddrRange range_{0, 1 << 30}; // cpu allocate addr range for cxlssd device
  //  Memory mem_;
  Tick latency_;           /**< DRAM access latency at the CXL-SSD device (50 ns)          */
  Tick cxl_mem_latency_;  /**< PCIe + CXL.mem protocol overhead — L_byte component (250 ns) */
  Tick pcie_latency_;     /**< PCIe DMA setup overhead for NAND block ops — L_block (2280 ns)*/

  SimpleSSD::HIL::HIL *pHIL{nullptr};

  bool enableCobraPolicy_; /**< true = COBRA Phase 2 SDT engine; false = pure block-I/O baseline */

  // CXL_SSD_CAPACITY mapped region
  int data_fd_{-1};
  uint64_t cache_hit_counts_{0};
  uint64_t access_counts_{0};
  char *mapped_cache_{nullptr};

  uint64_t instruction_id{0};
  uint32_t logical_page_size_{CXL_SSD_PAGE_SIZE};
  // uint32_t logical_page_size_{1 << 10};

  uint64_t capacity{CXL_SSD_CAPACITY};
  uint64_t cache_capacity{CXL_SSD_CACHE_CAPACITY};

  uint64_t pages_counts{CXL_SSD_CACHE_CAPACITY / CXL_SSD_PAGE_SIZE};

  Addr physicalAddrToSSDAddr(Addr addr) { return addr - range_.start(); };

  Page *pages{nullptr};
  EvictStrategy *evict_strategy{nullptr};

public:
  virtual Tick read(PacketPtr pkt) override;
  virtual Tick write(PacketPtr pkt) override;

  void access(PacketPtr pkt);
  virtual AddrRangeList getAddrRanges() const override;

  Tick resolve_cxl_mem(PacketPtr ptk);

  // for cxl-ssd
  uint8_t *toHostAddr(Addr addr);
  bool ssdAddrCheck(PacketPtr &ptk);
  Tick ssdRead(PacketPtr pkt);
  Tick ssdWrite(PacketPtr pkt);
  uint64_t GetPagesIndex(Addr ssd_start) const;

  /* ── BWB ring buffer ─────────────────────────────────────────── */
  uint8_t    *bwb_data_{nullptr};              /**< 256MB contiguous data payload ring buffer */
  BwbMetaSet  bwb_meta_[BWB_META_SETS]{};     /**< 256-set × 4-way SRAM metadata array       */

  /**
   * Per-physical-slot reverse LBA map.
   * bwb_slot_lba_[slot] gives the LBA written to physical slot @p slot.
   * Updated by bwbWrite(); read by GC for O(1) head→LBA resolution.
   */
  uint64_t bwb_slot_lba_[BWB_MAX_SLOTS]{};

  /**
   * Log_Tail: next free Log_Index.  Advanced by fetch_add(1) on every write.
   * Log_Head: oldest valid Log_Index.  Advanced by GC drain.
   * Both are 32-bit unsigned; unsigned subtraction gives correct fill even
   * after wrap-around.
   */
  std::atomic<uint32_t> log_tail_{0};
  std::atomic<uint32_t> log_head_{0};

  /* ── Background GC state ──────────────────────────────────────── */
  bool                 gcActive_{false}; /**< true while BWB fill is above HWM  */
  EventFunctionWrapper gcEvent_;         /**< gem5 recurring poll event          */

  /* ── BWB internal helpers ─────────────────────────────────────── */

  /** Returns the number of occupied slots (saturates at BWB_MAX_SLOTS). */
  uint32_t bwbFillSlots() const;

  /**
   * Allocate the next ring buffer slot, copy @p size bytes from @p data,
   * and record metadata for @p lba.
   */
  void bwbWrite(uint64_t lba, const uint8_t *data, uint64_t size,
                uint64_t dirty_bitmap = ~0ULL);

  /**
   * Look up the most-recent metadata entry for @p lba.
   * Returns nullptr if the LBA is not currently in the BWB.
   * O(BWB_META_WAYS) = O(1).
   */
  BwbSlotMeta *bwbLookup(uint64_t lba);

  /**
   * Allocate a metadata entry for @p lba using LRU replacement.
   * If an invalid way exists it is returned immediately.  Otherwise the
   * way with the lowest lru_counter is chosen as victim, its data is
   * flushed to NAND via pHIL->write(), and the slot is reused.
   * Returns a pointer to the newly initialised (valid=true, lba set,
   * dirty_bitmap=0) entry.  Caller must set log_index and dirty_bitmap.
   */
  BwbSlotMeta *bwbAllocate(uint64_t lba);

  /**
   * Logically invalidate all metadata entries for @p lba.
   * O(BWB_META_WAYS) = O(1).  Data in the ring buffer is NOT zeroed;
   * the slot will be silently reused when Log_Tail wraps around.
   */
  void bwbInvalidate(uint64_t lba);

  /**
   * Handle a read from BAR2 (BWB MMIO control register page).
   * Decodes the register offset, populates the packet, and returns
   * the CXL.mem protocol latency.
   */
  Tick handleBar2Read(PacketPtr pkt);

  /* ── Background GC ────────────────────────────────────────────── */

  /** Schedule the next GC wakeup BWB_GC_POLL_TICKS from now. */
  void scheduleGcPoll();

  /**
   * One GC wakeup: check watermarks, drain up to BWB_GC_BATCH slots from
   * Log_Head, validate each against bwb_meta_, flush live slots to NAND,
   * and reschedule itself.
   */
  void runGcStep();

  /* ── I/O Merging helpers ──────────────────────────────────────── */

  /**
   * For each bit set in @p dirty_bitmap, copy the corresponding 64-byte
   * cacheline from physical ring-buffer slot @p slot into @p dst.
   * @p dst must point to the base of the 4KB page.  O(1) — iterates at
   * most 64 bits.
   */
  void bwbOverlayCachelines(uint8_t *dst, uint32_t slot,
                             uint64_t dirty_bitmap) const;

  /* ── COBRA Phase 2: SDT Snoop + Sync ─────────────────────────────── */

  /**
   * snoopClwb — called for every WritebackClean (CLWB) packet on BAR0.
   * Translates the physical address to an LBA + cacheline bit and ORs
   * it into the BWB metadata dirty_bitmap for that page.  If no entry
   * exists yet for the LBA, a new BWB slot is allocated.  Returns
   * immediately without issuing any NAND I/O — this is the byte path.
   */
  void snoopClwb(Addr paddr, size_t size);

  /**
   * handleSync — triggered by a Flush/Sync command (fsync).
   * Iterates the ENTIRE metadata array, flushes every valid BWB entry
   * to NAND, then clears all valid bits and resets the ring pointers.
   * Strictly full-BWB flush — partial-offset logic is forbidden.
   */
  void handleSync();

  /* ── Storage statistics ──────────────────────────────────────────────
   * Collected for WAF analysis and COBRA efficiency measurement.
   * WAF = nandPhysicalBytesWritten / hostLogicalBytesWritten
   * ─────────────────────────────────────────────────────────────── */
  struct CxlSsdStats : public statistics::Group {
    CxlSsdStats(statistics::Group *parent);

    /** Logical bytes written by the host (CLWB byte-path + NVMe block-path). */
    statistics::Scalar hostLogicalBytesWritten;

    /** Physical bytes written to NAND flash (all paths: GC, sync, victim evict, block DMA). */
    statistics::Scalar nandPhysicalBytesWritten;

    /** 4KB pages routed to NAND: N > N_BASE or no SDT entry (dense/background). */
    statistics::Scalar sdtDenseBypasses;

    /** 4KB pages absorbed into BWB: N ≤ N_BASE (sparse/foreground, NAND skipped). */
    statistics::Scalar sdtSparseAbsorptions;

    /** LRU victim evictions in bwbAllocate() that triggered a forced NAND write. */
    statistics::Scalar bwbVictimFlushes;

    /** NAND read-modify-write cycles: read-before-write on ssdWrite() cache miss. */
    statistics::Scalar nandRmwCounts;
  } stats_;

  using Param = CxlMemoryParams;
  CxlMemory(const Param &p);
  ~CxlMemory();
  void startup() override;
};

} // namespace gem5