#include "dev/storage/cxl_memory.hh"
#include "base/trace.hh"
#include "debug/CxlMemory.hh"
#include "debug/CxlMemoryCoherency.hh"
#include <cstdio>

namespace gem5 {

/**
 * Global Variables & SimpleSSD Engine Setup
 */
Engine engine;
std::ostream *pDebugLog = nullptr;
SimpleSSD::ConfigReader ssdConfig =
    initSimpleSSDEngine(&engine, pDebugLog, pDebugLog,
                        "./src/dev/storage/simplessd/config/sample.cfg");
/**
 * eventengine for simplessd
 */
Engine::Engine() : SimpleSSD::Simulator(), simTick(0), counter(0), eventHandled(0) {}
Engine::~Engine() {}

bool Engine::insertEvent(SimpleSSD::Event eid, uint64_t tick, uint64_t *pOldTick) {
  bool found = false;
  bool flag = false;
  auto insert = eventQueue.end();

  for (auto iter = eventQueue.begin(); iter != eventQueue.end(); iter++) {
    if (iter->first == eid) {
      found = true;
      if (pOldTick) *pOldTick = iter->second;
      eventQueue.erase(iter); // Remove old event
      break; 
    }
  }

  // Re-find insertion point after erasure or if new
  for (auto iter = eventQueue.begin(); iter != eventQueue.end(); iter++) {
    if (iter->second > tick) {
      insert = iter;
      flag = true;
      break;
    }
  }

  // Insert event
  eventQueue.insert(insert, {eid, tick});
  return found;
}

bool Engine::removeEvent(SimpleSSD::Event eid) {
  for (auto iter = eventQueue.begin(); iter != eventQueue.end(); iter++) {
    if (iter->first == eid) {
      eventQueue.erase(iter);
      return true;
    }
  }
  return false;
}

bool Engine::isEventExist(SimpleSSD::Event eid, uint64_t *pTick) {
  for (auto &iter : eventQueue) {
    if (iter.first == eid) {
      if (pTick) *pTick = iter.second;
      return true;
    }
  }
  return false;
}

uint64_t Engine::getCurrentTick() { return simTick; }

SimpleSSD::Event Engine::allocateEvent(SimpleSSD::EventFunction func) {
  auto iter = eventList.insert({++counter, func});
  if (!iter.second) SimpleSSD::ssd_panic("Fail to allocate event");
  return counter;
}

void Engine::scheduleEvent(SimpleSSD::Event eid, uint64_t tick) {
  auto iter = eventList.find(eid);
  if (iter != eventList.end()) {
    if (tick < simTick) tick = simTick;
    insertEvent(eid, tick, nullptr);
  } else {
    SimpleSSD::ssd_panic("Event %" PRIu64 " does not exists", eid);
  }
}

void Engine::descheduleEvent(SimpleSSD::Event eid) {
  if (eventList.find(eid) != eventList.end()) {
    removeEvent(eid);
  } else {
    SimpleSSD::ssd_panic("Event %" PRIu64 " does not exists", eid);
  }
}

bool Engine::isScheduled(SimpleSSD::Event eid, uint64_t *pTick) {
  if (eventList.find(eid) != eventList.end()) {
    return isEventExist(eid, pTick);
  } else {
    SimpleSSD::ssd_panic("Event %" PRIu64 " does not exists", eid);
  }
  return false;
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

// ==========================================
// BiTieredCache Implementation
// ==========================================

BiTieredCache::BiTieredCache(uint8_t t_iso, uint8_t t_dist, std::function<void(Addr, uint8_t*)> flush_cb) 
    : threshold_isolated_(t_iso), 
      threshold_distributed_(t_dist),
      flush_callback(flush_cb) {
    
    classify_queue = new FIFOQueue<logical_frame_t, ClassifyNode>(NUM_CLASSIFY_PAGES);
    store_queue = new FIFOQueue<logical_chunk_t, ChunkNode>(NUM_STORE_CHUNKS);
    dirty_queue = new FIFOQueue<logical_chunk_t, ChunkNode>(NUM_DIRTY_CHUNKS);

    // Initialize Free Lists
    for(phys_index_t i = 0; i < NUM_CLASSIFY_PAGES; ++i) free_classify_indices.push(i);
    for(phys_index_t i = 0; i < NUM_STORE_CHUNKS; ++i) free_store_indices.push(i);
    for(phys_index_t i = 0; i < NUM_DIRTY_CHUNKS; ++i) free_dirty_indices.push(i);
}

BiTieredCache::~BiTieredCache() {
    delete classify_queue;
    delete store_queue;
    delete dirty_queue;
}

// Helper to calculate byte offset in the 2GB file
uint64_t BiTieredCache::GetPhysicalOffset(phys_index_t idx, int area_type) {
    uint64_t classify_size = (uint64_t)NUM_CLASSIFY_PAGES * CXL_SSD_PAGE_SIZE; // 1GB
    uint64_t store_size = (uint64_t)NUM_STORE_CHUNKS * CXL_MEM_CHUNK_SIZE;      // 512MB
    
    if (area_type == 0) { // Classify
        return idx * CXL_SSD_PAGE_SIZE;
    } else if (area_type == 1) { // Store
        return classify_size + (idx * CXL_MEM_CHUNK_SIZE);
    } else { // Dirty
        return classify_size + store_size + (idx * CXL_MEM_CHUNK_SIZE);
    }
}

// Allocator Helpers
phys_index_t BiTieredCache::AllocateClassifyIndex() {
    if(free_classify_indices.empty()) return (phys_index_t)-1;
    phys_index_t idx = free_classify_indices.front();
    free_classify_indices.pop();
    return idx;
}
phys_index_t BiTieredCache::AllocateStoreIndex() {
    if(free_store_indices.empty()) return (phys_index_t)-1;
    phys_index_t idx = free_store_indices.front();
    free_store_indices.pop();
    return idx;
}
phys_index_t BiTieredCache::AllocateDirtyIndex() {
    if(free_dirty_indices.empty()) return (phys_index_t)-1;
    phys_index_t idx = free_dirty_indices.front();
    free_dirty_indices.pop();
    return idx;
}

// Core Read Logic
AccessStatus BiTieredCache::HandleRead(Addr addr, uint32_t size, char* base_ptr) {
    // Note: We assume requests coming here are fine-grained (<= 256B) 
    // or we break larger requests into chunks. 
    // Here we strictly handle the address as a specific chunk lookup.
    
    logical_frame_t lpn = addr / CXL_SSD_PAGE_SIZE;
    logical_chunk_t lcn = addr / CXL_MEM_CHUNK_SIZE;
    chunk_index_t offset_in_page = (addr % CXL_SSD_PAGE_SIZE) / CXL_MEM_CHUNK_SIZE;

    // 1. Check Dirty
    if (dirty_queue->Contains(lcn)) {
        DPRINTF(CxlMemory, "Read HIT in Dirty: LCN %lu\n", lcn);
        return AccessStatus::HIT; 
    }
    // 2. Check Store
    ChunkNode* s_node = store_queue->Get(lcn);
    if (store_queue->Contains(lcn)) {
      DPRINTF(CxlMemory, "Read HIT in Store: LCN %lu\n", lcn);

      // 避免 uint8_t access_count 溢位
      if (s_node->access_count < 255) s_node->access_count++;

      // 檢查 Isolated Hotspot
      if (s_node->access_count > threshold_isolated_) {
        DPRINTF(CxlMemory, "Anomaly (Isolated) in Store: LCN %lu\n", lcn);
        // 論文: "For such cases, the entire 4KB page must be reloaded..."
        // 這裡回傳 MIGRATE 信號，CxlMemory 會模擬這個 reload + migrate 的延遲
        return AccessStatus::ANOMALY_MIGRATE;
      }

      return AccessStatus::HIT;
    }
    // 3. Check Classify
    ClassifyNode* node = classify_queue->Get(lpn);
    if (node) {
        DPRINTF(CxlMemory, "Read HIT in Classify: LPN %lu\n", lpn);
        node->AccessChunk(offset_in_page);

        // Anomaly Detection
        if (node->access_counts[offset_in_page] > threshold_isolated_) return AccessStatus::ANOMALY_MIGRATE;
        
        int unique = 0;
        for(int i=0; i < CXL_MEM_CHUNKS_PER_PAGE; ++i) if((node->chunk_bitmap >> i) & 1) unique++;
        if (unique > threshold_distributed_) return AccessStatus::ANOMALY_MIGRATE;

        return AccessStatus::HIT;
    }
    return AccessStatus::MISS;
}

// Core Write Logic
AccessStatus BiTieredCache::HandleWrite(Addr addr, uint32_t size, const uint8_t* data, char* base_ptr) {
  logical_frame_t lpn = addr / CXL_SSD_PAGE_SIZE;
  logical_chunk_t lcn = addr / CXL_MEM_CHUNK_SIZE;
  chunk_index_t offset_in_page = (addr % CXL_SSD_PAGE_SIZE) / CXL_MEM_CHUNK_SIZE;

  // 1. If in Classify -> Move to Dirty
  ClassifyNode* c_node = classify_queue->Get(lpn);
  if (c_node) { // Copy data from Classify Area (4KB) to temp buffer
    //  Updates count towards Anomaly
    c_node->AccessChunk(offset_in_page);

    // Check Anomaly: Distributed Hotspot
    // (寫入通常會增加 unique chunk count，容易觸發 Distributed)
    int unique = 0;
    for(int i=0; i < CXL_MEM_CHUNKS_PER_PAGE; ++i) {
      if((c_node->chunk_bitmap >> i) & 1) unique++;
    }

    if (unique > threshold_distributed_) {
      DPRINTF(CxlMemory, "Anomaly (Distributed) on Write: LPN %lu. Migrating to Host.\n", lpn);
      return AccessStatus::ANOMALY_MIGRATE;
    }

    // 若無 Anomaly，則執行 "Move to Dirty" 
    uint64_t src_off = GetPhysicalOffset(c_node->phys_index, 0) + (offset_in_page * CXL_MEM_CHUNK_SIZE);
    std::vector<uint8_t> buf(CXL_MEM_CHUNK_SIZE);
    std::memcpy(buf.data(), base_ptr + src_off, CXL_MEM_CHUNK_SIZE); // Copy old data
    if (size <= CXL_MEM_CHUNK_SIZE) std::memcpy(buf.data(), data, size); // Apply update

    MoveToDirty(lcn, buf, base_ptr);
    return AccessStatus::HIT; // Hit & Handled
  }

  // 2. Store -> Move to Dirty
  ChunkNode* s_node = store_queue->Get(lcn);
  if (s_node) {
    // 避免 uint8_t access_count 溢位
    if (s_node->access_count < 255) s_node->access_count++;

    if (s_node->access_count > threshold_isolated_) {
      DPRINTF(CxlMemory, "Anomaly (Isolated) on Write in Store: LCN %lu\n", lcn);
      return AccessStatus::ANOMALY_MIGRATE;
    }

    // 若無 Anomaly，才執行 Move to Dirty
    uint64_t src_off = GetPhysicalOffset(s_node->phys_index, 1);
    std::vector<uint8_t> buf(CXL_MEM_CHUNK_SIZE);
    std::memcpy(buf.data(), base_ptr + src_off, CXL_MEM_CHUNK_SIZE);
    if (size <= CXL_MEM_CHUNK_SIZE) std::memcpy(buf.data(), data, size);

    // Remove from Store & Free Index
    phys_index_t s_idx = s_node->phys_index;
    store_queue->Remove(lcn);
    FreeStoreIndex(s_idx);

    MoveToDirty(lcn, buf, base_ptr);
    return AccessStatus::HIT;
  }

  // 3. Dirty -> Update
  ChunkNode* d_node = dirty_queue->Get(lcn);
  if (d_node) {
      uint64_t offset = GetPhysicalOffset(d_node->phys_index, 2);
      std::memcpy(base_ptr + offset, data, size);
      return AccessStatus::HIT;
  }

  // 4. Miss
  return AccessStatus::MISS;
}

// Called on Read Miss: Insert fetched page into Classify, Strict FIFO Eviction Logic
void BiTieredCache::InsertToClassify(Addr addr, const std::vector<uint8_t>& page_data, char* base_ptr) {
  logical_frame_t lpn = addr / CXL_SSD_PAGE_SIZE;
  phys_index_t p_idx;
  
  if (classify_queue->IsFull()) {
      // Evict Oldest (Victim)
      ClassifyNode& victim_ref = classify_queue->PeekFront(); // Peek before remove
      
      // Move valid chunks to Store
      MoveToStore(victim_ref, base_ptr);
      
      // Reuse the physical index
      p_idx = victim_ref.phys_index;
      
      // Now actually remove from logical queue
      classify_queue->Remove(victim_ref.logical_frame);
  } else {
      p_idx = AllocateClassifyIndex();
      if (p_idx == (phys_index_t)-1) return; // Should be covered by IsFull check
  }
  
  // Write Data
  uint64_t offset = GetPhysicalOffset(p_idx, 0);
  std::memcpy(base_ptr + offset, page_data.data(), CXL_SSD_PAGE_SIZE);

  // Insert
  ClassifyNode new_node(lpn, p_idx);
  classify_queue->Insert(lpn, new_node);
}

void BiTieredCache::MoveToStore(const ClassifyNode& node, char* base_ptr) {
    for (int i = 0; i < CXL_MEM_CHUNKS_PER_PAGE; ++i) {
        if (node.chunk_bitmap & (1 << i)) {
            logical_chunk_t lcn = (node.logical_frame * CXL_MEM_CHUNKS_PER_PAGE) + i;
            phys_index_t s_idx;

            // Check if already in Store to avoid duplicates (though unlikely with this logic flow)
            if (store_queue->Contains(lcn)) continue;

            if (store_queue->IsFull()) {
                // Evict from Store (FIFO)
                ChunkNode& victim = store_queue->PeekFront();
                s_idx = victim.phys_index; // Reuse index
                store_queue->Remove(victim.logical_chunk_addr);
            } else {
                s_idx = AllocateStoreIndex();
            }

            // Copy Data
            uint64_t src = GetPhysicalOffset(node.phys_index, 0) + (i * CXL_MEM_CHUNK_SIZE);
            uint64_t dst = GetPhysicalOffset(s_idx, 1);
            std::memcpy(base_ptr + dst, base_ptr + src, CXL_MEM_CHUNK_SIZE);

            store_queue->Insert(lcn, ChunkNode(lcn, s_idx));
        }
    }
}

void BiTieredCache::MoveToDirty(logical_chunk_t lcn, const std::vector<uint8_t>& data, char* base_ptr) {
   phys_index_t d_idx;
    
    // Check if already exists (update only)
    ChunkNode* existing = dirty_queue->Get(lcn);
    if (existing) {
        uint64_t offset = GetPhysicalOffset(existing->phys_index, 2);
        std::memcpy(base_ptr + offset, data.data(), CXL_MEM_CHUNK_SIZE);
        return;
    }

    if (dirty_queue->IsFull()) {
        // Evict from Dirty (FIFO) -> Flush
        ChunkNode& victim = dirty_queue->PeekFront();
        
        // Read data
        uint64_t v_off = GetPhysicalOffset(victim.phys_index, 2);
        std::vector<uint8_t> v_data(CXL_MEM_CHUNK_SIZE);
        std::memcpy(v_data.data(), base_ptr + v_off, CXL_MEM_CHUNK_SIZE);

        // Flush callback to SSD
        flush_callback(victim.logical_chunk_addr * CXL_MEM_CHUNK_SIZE, v_data.data());
        
        d_idx = victim.phys_index; // Reuse index
        dirty_queue->Remove(victim.logical_chunk_addr);
    } else {
        d_idx = AllocateDirtyIndex();
    }

    // Write new data
    uint64_t offset = GetPhysicalOffset(d_idx, 2);
    std::memcpy(base_ptr + offset, data.data(), CXL_MEM_CHUNK_SIZE);

    dirty_queue->Insert(lcn, ChunkNode(lcn, d_idx));
}



// ==========================================
// CxlMemory Implementation
// ==========================================

CxlMemory::CxlMemory(const Param &p)
    : DmaDevice(p), 
      latency_(p.latency), 
      cxl_mem_latency_(p.cxl_mem_latency),
      pHIL(new SimpleSSD::HIL::HIL(ssdConfig)) {
      
  range_ = AddrRange(0x100000000, 0x100000000 + CXL_SSD_CAPACITY);

  data_fd_ = open("./CxlSSD.img", O_RDWR | O_CREAT | O_TRUNC, 0666);
  if (data_fd_ == -1) {
    perror("Error opening file");
    assert(0);
  }
  if (ftruncate(data_fd_, CXL_SSD_CAPACITY) == -1) {
    perror("Error setting file size");
    assert(0);
  }

  mapped_cache_ = (char *)mmap(NULL, CXL_SSD_CAPACITY, PROT_READ | PROT_WRITE, MAP_SHARED, data_fd_, 0);
  if (mapped_cache_ == MAP_FAILED) {
    perror("Error mmap");
    exit(1);
  }

  size_t host_cache_mb = p.host_cache_size / (1024 * 1024);
  host_cache_tracker = new HostCacheTracker(host_cache_mb);
  uint8_t t_iso = p.threshold_isolated;
  uint8_t t_dist = p.threshold_distributed;

  // Init Cache with Callback to this->internalFlush
  haipc = new BiTieredCache(t_iso, t_dist, [this](Addr addr, uint8_t* data) {
      this->internalFlush(addr, data);
  });
}

CxlMemory::~CxlMemory() {
  delete pHIL;
  delete haipc;
  munmap(mapped_cache_, CXL_SSD_CAPACITY);
  close(data_fd_);
}

uint8_t *CxlMemory::toHostAddr(Addr addr) {
    return (uint8_t *)mapped_cache_ + physicalAddrToSSDAddr(addr);
}

void CxlMemory::access(PacketPtr pkt) {
    // BAR handling for PCI config
    // range_ = AddrRange(BARs[0]->addr(), BARs[0]->addr() + BARs[0]->size());

    // Note: In CXL Type-3, we usually respond to Mem access. 
    // Checking cacheResponding() prevents double response.
    if (pkt->cacheResponding()) return;
    if (pkt->cmd == MemCmd::CleanEvict || pkt->cmd == MemCmd::WritebackClean) return;
    
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
        bool overwrite_mem = true;

        pkt->writeData(&overwrite_val[0]);
        pkt->setData(host_addr);

        if (pkt->req->isCondSwap()) {
            if (pkt->getSize() == sizeof(uint64_t)) {
                condition_val64 = pkt->req->getExtraData();
                overwrite_mem = !std::memcmp(&condition_val64, host_addr, sizeof(uint64_t));
            } else if (pkt->getSize() == sizeof(uint32_t)) {
                condition_val32 = (uint32_t)pkt->req->getExtraData();
                overwrite_mem = !std::memcmp(&condition_val32, host_addr, sizeof(uint32_t));
            }
        }
        if (overwrite_mem) std::memcpy(host_addr, &overwrite_val[0], pkt->getSize());
      }
    } else if (pkt->isRead()) {
        if (mapped_cache_) pkt->setData(host_addr);
    } else if (pkt->isWrite()) {
        if (mapped_cache_) pkt->writeData(host_addr);
    }

    if (pkt->needsResponse()) pkt->makeResponse();
}

Tick CxlMemory::read(PacketPtr pkt) {
    // Perform functional access first (get data into packet)
    access(pkt);
    
    Addr addr = physicalAddrToSSDAddr(pkt->getAddr());
    logical_frame_t lpn = addr / CXL_SSD_PAGE_SIZE;
    uint32_t size = pkt->getSize();
    
    // 檢查是否已經遷移到 Host DRAM
    if (host_cache_tracker->TryAccess(lpn)) {
      DPRINTF(CxlMemory, "Read Redirect to Host DRAM (Simulated): LPN %lu\n", lpn);
      // 這裡回傳 Host DRAM 的延遲，模擬 "Hit in Host Page Cache"
      // 雖然 Packet 實際上是走到 CXL Device，但我們假裝它是從 Host DRAM 回來的
      return host_dram_latency_; 
    }
    
    // 1. Try HAIPC
    AccessStatus status = haipc->HandleRead(addr, size, mapped_cache_);

    if (status == AccessStatus::HIT) return cxl_mem_latency_ + latency_;
    if (status == AccessStatus::ANOMALY_MIGRATE) {
      host_cache_tracker->Insert(lpn);
      return cxl_mem_latency_ + latency_ + ssdRead(pkt) + migration_penalty_;
    }

    // 2. Load to Classify Area
    // We need to fetch the whole 4KB page that contains this addr
    // Simulation: We just create a dummy buffer or read from mmap if SSD backend updated it
    std::vector<uint8_t> page_data(CXL_SSD_PAGE_SIZE, 0); 
    // In a real impl, ssdRead would populate a buffer. 
    
    haipc->InsertToClassify(addr, page_data, mapped_cache_);

    return cxl_mem_latency_ + latency_ + ssdRead(pkt);
}

Tick CxlMemory::write(PacketPtr pkt) {
  access(pkt);

  Addr addr = physicalAddrToSSDAddr(pkt->getAddr());
  logical_frame_t lpn = addr / CXL_SSD_PAGE_SIZE;
  uint32_t size = pkt->getSize();
  const uint8_t* data = pkt->getConstPtr<uint8_t>();

  // 檢查是否已遷移
  if (host_cache_tracker->TryAccess(lpn)) {
    DPRINTF(CxlMemory, "Write Redirect to Host DRAM (Simulated Hit): LPN %lu\n", lpn);
    return host_dram_latency_; // 模擬 Host 處理寫入
  }

  // 1. Try HAIPC Update
  AccessStatus status = haipc->HandleWrite(addr, size, data, mapped_cache_);

  if (status == AccessStatus::HIT) {
    return cxl_mem_latency_ + latency_;
  } 
  else if (status == AccessStatus::ANOMALY_MIGRATE) {
    // 處理寫入時的遷移
    host_cache_tracker->Insert(lpn);
    // 這裡回傳 CXL Latency + Penalty，因為這次寫入實際上觸發了搬移
    return cxl_mem_latency_ + latency_ + ssdWrite(pkt) + migration_penalty_;
  }

  // 2. Miss -> Flash Write
  return cxl_mem_latency_ + latency_ + ssdWrite(pkt);
}

void CxlMemory::internalFlush(Addr ssd_addr, uint8_t* data) {
  uint64_t latency = 0;
  SimpleSSD::HIL::Request request(&latency);
  request.reqID = ++instruction_id;
  request.range.slpn = ssd_addr / CXL_SSD_PAGE_SIZE;
  request.range.nlp = 1;
  request.offset = ssd_addr % CXL_SSD_PAGE_SIZE;
  request.length = CXL_MEM_CHUNK_SIZE;
  request.function = [](uint64_t, void *) {};
  
  pHIL->write(request);
}

Tick CxlMemory::ssdRead(PacketPtr pkt) {
  uint64_t latency = 0;
  SimpleSSD::HIL::Request request(&latency);
  request.reqID = ++instruction_id;
  request.range.slpn = physicalAddrToSSDAddr(pkt->getAddr()) / CXL_SSD_PAGE_SIZE;
  request.range.nlp = 1;
  request.offset = 0;
  request.length = CXL_SSD_PAGE_SIZE; // Reading full page from Flash
  request.function = [](uint64_t, void *) {};
  pHIL->read(request);
  return latency;
}

Tick CxlMemory::ssdWrite(PacketPtr pkt) {
    uint64_t latency = 0;
    SimpleSSD::HIL::Request request(&latency);
    request.reqID = ++instruction_id;
    request.range.slpn = physicalAddrToSSDAddr(pkt->getAddr()) / CXL_SSD_PAGE_SIZE;
    request.range.nlp = 1;
    request.offset = 0;
    request.length = CXL_SSD_PAGE_SIZE;
    request.function = [](uint64_t, void *) {};
    pHIL->write(request);
    return latency;
}

Tick CxlMemory::resolve_cxl_mem(PacketPtr pkt) { return cxl_mem_latency_; }
AddrRangeList CxlMemory::getAddrRanges() const { 
  AddrRangeList ranges;
  ranges.push_back(range_);
  return ranges;
}

} // namespace gem5


