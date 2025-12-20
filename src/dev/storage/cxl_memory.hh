#include "base/addr_range.hh"
#include "base/bitfield.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/CxlMemory.hh"
#include "debug/CxlMemoryCacheHit.hh"
#include "debug/CxlMemoryCoherency.hh"
#include "dev/pci/device.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "params/CxlMemory.hh"
#include "dev/storage/evict_strategy.hh"
#include "dev/storage/simple_ssd.hh"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <list>

namespace gem5 {

#define CXL_SSD_PAGE_LEFT_BITS (10)                       // 10 bit
#define CXL_SSD_CAPACITY (1LL << 32)                      // 4G capacity
#define CXL_SSD_CACHE_CAPACITY (1LL << 24)                // 32M capacity
#define CXL_SSD_PAGE_SIZE (1LL << CXL_SSD_PAGE_LEFT_BITS) // 4K capacity
#define CXL_SSD_CACHE_HIT_STAT (1 << 16)                  // 10 0000 counts
// #define CXL_MEMORY_ENABLE 1
// #define CXL_SSD_NO_CACHE 1

typedef size_t frame_id_t;

// ============================================================================
// Helper Structures
// ============================================================================

/**
 * Helper for building SimpleSSD requests
 */
struct SSDRequestBuilder {
  uint64_t& instruction_id;
  SimpleSSD::HIL::HIL* pHIL;
  uint64_t logical_page_size;
  
  SSDRequestBuilder(uint64_t& id, SimpleSSD::HIL::HIL* hil, uint64_t page_size)
      : instruction_id(id), pHIL(hil), logical_page_size(page_size) {}
  
  /**
   * Issue a write request (for writeback)
   * @return Write latency
   */
  uint64_t issueWriteRequest(uint64_t addr, uint64_t size) {
    uint64_t write_latency = 0;
    SimpleSSD::HIL::Request request(&write_latency);
    request.reqID = ++instruction_id;
    request.range.slpn = addr / logical_page_size;
    request.range.nlp = size / logical_page_size;
    request.offset = addr % logical_page_size;
    request.length = size;
    request.function = [](uint64_t, void *) {};
    request.context = (void *)instruction_id;
    pHIL->write(request);
    return write_latency;
  }
  
  /**
   * Issue a read request
   * @return Read latency
   */
  uint64_t issueReadRequest(uint64_t addr, uint64_t size) {
    uint64_t read_latency = 0;
    SimpleSSD::HIL::Request request(&read_latency);
    request.reqID = ++instruction_id;
    request.range.slpn = addr / logical_page_size;
    request.range.nlp = size / logical_page_size;
    request.offset = addr % logical_page_size;
    request.length = size;
    request.function = [](uint64_t, void *) {};
    request.context = (void *)instruction_id;
    pHIL->read(request);
    return read_latency;
  }
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

class CxlMemory : public PciDevice {
private:
  AddrRange range_{0, 1 << 30}; // cpu allocate addr range for cxlssd device
  Tick latency_;
  Tick cxl_mem_latency_;

  SimpleSSD::HIL::HIL *pHIL{nullptr};

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

  // ========== Helper Methods ==========
  
  /**
   * Check cache hit and update page metadata
   * @return True if cache hit, false if miss
   */
  bool checkAndUpdatePageCache(uint64_t logical_frame, uint64_t ssd_start);
  
  /**
   * Perform page writeback if dirty
   * @return Writeback latency in ticks
   */
  Tick performPageWriteback(uint64_t page_index);
  
  /**
   * Build and execute SSD read request
   * @return Read latency in ticks
   */
  uint64_t executeReadRequest(uint64_t ssd_start, uint64_t size);
  
  /**
   * Build and execute SSD write request
   * @return Write latency in ticks
   */
  uint64_t executeWriteRequest(uint64_t ssd_start, uint64_t size);

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

  using Param = CxlMemoryParams;
  CxlMemory(const Param &p);
  ~CxlMemory();
};

} // namespace gem5