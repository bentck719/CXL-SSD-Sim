#include "base/addr_range.hh"
#include "base/bitfield.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/CxlMemory.hh"
#include "debug/CxlMemoryCacheHit.hh"
#include "debug/CxlMemoryCoherency.hh"
#include "dev/dma_device.hh"
#include "dev/storage/simplessd/hil/hil.hh"
#include "dev/storage/simplessd/util/simplessd.hh"
#include "dev/storage/fifo_queue.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "params/CxlMemory.hh"
#include <fcntl.h>
#include <set>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace gem5 {

// --- 1. Constants Correction ---
#define CXL_SSD_CAPACITY        (16ULL * 1024 * 1024 * 1024) // 16GB Total File Size (Example)
#define CXL_SSD_CACHE_CAPACITY  (1LL << 31) // 2GB Device DRAM
#define CXL_SSD_PAGE_SIZE       (4096)      // 4KB Page Size
#define CXL_MEM_CHUNK_SIZE      (256)       // 256B Chunk Size
#define CXL_MEM_CHUNKS_PER_PAGE (16)        // 16 Chunks per Page

// --- Memory Layout Boundaries (2GB Total) ---
// Classify Area: 1GB (Holds 4KB Pages) -> 262,144 Pages
// Store Area: 512MB (Holds 256B Chunks) -> 2,097,152 Chunks
// Dirty Area: 512MB (Holds 256B Chunks) -> 2,097,152 Chunks
#define NUM_CLASSIFY_PAGES (262144)
#define NUM_STORE_CHUNKS   (2097152)
#define NUM_DIRTY_CHUNKS   (2097152)

typedef uint64_t Tick;
typedef uint64_t logical_frame_t; // Page Address (LPN)
typedef uint64_t logical_chunk_t; // Chunk Address (LPN * 16 + offset)
typedef uint32_t chunk_index_t;   // 0-15
typedef uint64_t phys_index_t;    // Index in the mmap array

// Enum for Access Result
enum class AccessStatus {
    MISS,
    HIT,
    ANOMALY_MIGRATE // Signal to move to Host
};

// --- 2. Data Structures ---

// Node for Classify Area (Page granularity)
struct ClassifyNode {
    logical_frame_t logical_frame; // Key
    phys_index_t phys_index;       // Where in the 1GB buffer it lives
    uint16_t chunk_bitmap;         // 16 bits [cite: 334]
    uint8_t access_counts[CXL_MEM_CHUNKS_PER_PAGE]; // [cite: 335]

    ClassifyNode(logical_frame_t lpn, phys_index_t p_idx)
        : logical_frame(lpn), phys_index(p_idx), chunk_bitmap(0) {
        std::memset(access_counts, 0, sizeof(access_counts));
    }

    void AccessChunk(chunk_index_t idx) {
        chunk_bitmap |= (1 << idx);
        if (access_counts[idx] < 255) access_counts[idx]++;
    }
};

// Node for Store/Dirty Area (Chunk granularity)
struct ChunkNode {
    logical_chunk_t logical_chunk_addr; // Key
    phys_index_t phys_index;            // Where in the buffer it lives
    uint8_t access_count;               // 為了 Isolated Hotspot

    ChunkNode(logical_chunk_t addr, phys_index_t p_idx) 
        : logical_chunk_addr(addr), phys_index(p_idx), access_count(0) {}
};

// --- 3. Bi-Tiered Cache Controller ---
class BiTieredCache {
private:
    // Physical Memory Management (Indices)
    std::queue<phys_index_t> free_classify_indices;
    std::queue<phys_index_t> free_store_indices;
    std::queue<phys_index_t> free_dirty_indices;

    // Logical Queues
    FIFOQueue<logical_frame_t, ClassifyNode> *classify_queue;
    FIFOQueue<logical_chunk_t, ChunkNode> *store_queue;
    FIFOQueue<logical_chunk_t, ChunkNode> *dirty_queue;

    // Callback to flush data to SSD
    std::function<void(Addr, uint8_t*)> flush_callback;

    uint8_t threshold_isolated_;
    uint8_t threshold_distributed_;

    // Helpers
    phys_index_t AllocateClassifyIndex();
    phys_index_t AllocateStoreIndex();
    phys_index_t AllocateDirtyIndex();

    void FreeClassifyIndex(phys_index_t idx) { free_classify_indices.push(idx); }
    void FreeStoreIndex(phys_index_t idx) { free_store_indices.push(idx); }
    void FreeDirtyIndex(phys_index_t idx) { free_dirty_indices.push(idx); }

    // Internal Logic
    void MoveToStore(const ClassifyNode& node, char* base_ptr);
    void MoveToDirty(logical_chunk_t chunk_addr, const std::vector<uint8_t>& data, char* base_ptr);

public:
    BiTieredCache(uint8_t t_iso, uint8_t t_dist, std::function<void(Addr, uint8_t*)> flush_cb);
    ~BiTieredCache();

    AccessStatus HandleRead(Addr addr, uint32_t size, char* base_ptr);
    AccessStatus HandleWrite(Addr addr, uint32_t size, const uint8_t* data, char* base_ptr);
    void InsertToClassify(Addr addr, const std::vector<uint8_t>& page_data, char* base_ptr);

    uint64_t GetPhysicalOffset(phys_index_t idx, int area_type); 
};

// 用來模擬 Anomoly handler 把 in-device page cache 
// 移動到 host page cache 的管理
class HostCacheTracker {
private:
  size_t capacity_pages_;
  std::list<logical_frame_t> lru_list_;
  std::unordered_map<logical_frame_t, std::list<logical_frame_t>::iterator> map_;

public:
  // capacity_mb: Host Page Cache 大小 (例如 8GB)
  HostCacheTracker(size_t capacity_mb) {
    // 轉換 MB 為 Page 數量 (4KB per page)
    // 1 MB = 1024 * 1024 bytes
    // 1 Page = 4096 bytes
    // Pages per MB = 256
    capacity_pages_ = capacity_mb * 256; 
  }

  // 檢查是否存在，若存在則更新 LRU 位置 (模擬 Host Cache Hit)
  bool TryAccess(logical_frame_t lpn) {
    auto it = map_.find(lpn);
    if (it == map_.end()) {
        return false; // Miss
    }
    // Hit: Move to front (Most Recently Used)
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
    return true;
  }

  // 插入新 Page (模擬 Migration)
  void Insert(logical_frame_t lpn) {
    if (map_.count(lpn)) {
      TryAccess(lpn); // 已經存在，更新位置即可
      return;
    }

    // 檢查容量，若滿了則 Evict (模擬 Host Eviction)
    if (map_.size() >= capacity_pages_) {
      logical_frame_t victim = lru_list_.back();
      lru_list_.pop_back();
      map_.erase(victim);
      // 注意：這裡我們不需要通知 CXL Device。
      // 因為在真實世界中，Host 丟棄 Clean Page 是無聲的。
      // 下次 CPU 存取該 victim 時，TryAccess 會回傳 false，
      // 請求就會自然地流回 CXL Device。
    }

    lru_list_.push_front(lpn);
    map_[lpn] = lru_list_.begin();
  }
};

class CxlMemory : public DmaDevice {
private:
  AddrRange range_;
  Tick latency_;
  Tick cxl_mem_latency_;

  SimpleSSD::HIL::HIL *pHIL{nullptr};

  int data_fd_{-1};
  char *mapped_cache_{nullptr}; // Base pointer to 2GB mmap

  // The Logic Module
  BiTieredCache *haipc{nullptr}; 

  // Pages are logically moved to Host DRAM 
  HostCacheTracker* host_cache_tracker;

  // 定義 Host DRAM 的延遲 (比 CXL 快)
  // 論文數據: DDR5 4KB latency ~106ns
  Tick host_dram_latency_{106500}; // 假設 106.5 ns (以 gem5 tick 為單位，假設 1 tick = 1 ps)
  
  // 定義遷移成本 (Migration Penalty)
  // 讀取 4KB 並寫入 Host 的時間。假設頻寬 32GB/s -> 4KB 約需 125ns，加上協定開銷，設個保守值
  Tick migration_penalty_{500000}; // 500 ns

  uint64_t instruction_id{0};

  Addr physicalAddrToSSDAddr(Addr addr) { return addr - range_.start(); };
  uint8_t *toHostAddr(Addr addr);

public:
  virtual Tick read(PacketPtr pkt) override;
  virtual Tick write(PacketPtr pkt) override;
  virtual AddrRangeList getAddrRanges() const override;

  void access(PacketPtr pkt);
  Tick resolve_cxl_mem(PacketPtr ptk);
  Tick ssdRead(PacketPtr pkt);
  Tick ssdWrite(PacketPtr pkt);

  // Called by HAIPC to flush chunks
  void internalFlush(Addr ssd_addr, uint8_t* data);

  using Param = CxlMemoryParams;
  CxlMemory(const Param &p);
  ~CxlMemory();
};

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

extern Engine engine;
extern SimpleSSD::ConfigReader ssdConfig;

} // namespace gem5