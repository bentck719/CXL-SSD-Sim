#include "dev/storage/general_queue.hh"

#include <cstdint>
#include <vector>
#include <optional>
#include <cassert>

namespace gem5 {

#define CXL_SSD_PAGE_SIZE 4096

using logical_frame_t = uint64_t;
using frame_id_t = uint64_t;

enum class EvictStrategyMode { Direct, LRU, FIFO, TwoQ, LFRU };

// -----------------------------------------------------------------------------
// Helper: FrameAllocator
// 負責管理 Frame ID 的分配與回收。
// 這樣我們就不需要在 TwoQ 裡面手動搞那一堆 list 操作。
// -----------------------------------------------------------------------------
class FrameAllocator {
private:
    frame_id_t max_frames_;
    frame_id_t next_id_ = 0;
    std::vector<frame_id_t> free_list_;

public:
    explicit FrameAllocator(size_t max_frames) : max_frames_(max_frames) {
        free_list_.reserve(max_frames);
    }

    // 嘗試拿一個 ID，如果沒了回傳 nullopt
    std::optional<frame_id_t> allocate() {
        if (!free_list_.empty()) {
            frame_id_t id = free_list_.back();
            free_list_.pop_back();
            return id;
        }
        if (next_id_ < max_frames_) {
            return next_id_++;
        }
        return std::nullopt;
    }

    // 回收 ID
    void deallocate(frame_id_t id) {
        free_list_.push_back(id);
    }
};

// -----------------------------------------------------------------------------
// Strategy Base Class
// -----------------------------------------------------------------------------
class EvictStrategy {
public:
    virtual ~EvictStrategy() = default;
    virtual frame_id_t access(logical_frame_t logical_frame) = 0;
};

// -----------------------------------------------------------------------------
// Strategy: Direct Mapping
// -----------------------------------------------------------------------------
class DirectEvictStrategy : public EvictStrategy {
private:
    uint64_t page_counts_;

public:
    explicit DirectEvictStrategy(uint64_t page_counts) : page_counts_(page_counts) {}
    
    frame_id_t access(logical_frame_t logical_frame) override {
        uint64_t tag = logical_frame / CXL_SSD_PAGE_SIZE;
        return tag & (page_counts_ - 1);
    }
};

// -----------------------------------------------------------------------------
// Strategy: FIFO
// 使用 GeneralQueue 直接實作
// -----------------------------------------------------------------------------
class FIFOEvictStrategy : public EvictStrategy {
private:
    // 內部使用你的 GeneralQueue
    GeneralQueue<logical_frame_t, frame_id_t> queue_;
    FrameAllocator allocator_;

public:
    explicit FIFOEvictStrategy(size_t capacity) 
        : queue_(capacity / CXL_SSD_PAGE_SIZE),
          allocator_(capacity / CXL_SSD_PAGE_SIZE) {}

    frame_id_t access(logical_frame_t key) override {
        // 1. HIT: 如果存在，直接回傳 ID
        // FIFO 規則：Hit 不會改變順序，所以直接 Get 就好
        if (queue_.Contains(key)) {
            return *queue_.Get(key);
        }

        // 2. MISS: 需要分配新的 Frame ID
        frame_id_t target_fid;

        if (!queue_.IsFull()) {
            // A. 未滿：直接向 Allocator 要一個新的 ID
            target_fid = allocator_.allocate().value();
        } 
        else {
            // B. 已滿：重用最舊 (Head) 的 frame_id
            // 邏輯：先偷看最前面是誰，拿到他的 ID，然後 Insert 會自動把它踢掉
            auto victim_key = *queue_.PeekFront();
            target_fid = *queue_.Get(victim_key);
        }

        // 插入新資料 (如果是 case B，Insert 內部會自動 PopFront)
        queue_.Insert(key, target_fid);
        
        return target_fid;
    }
};

// -----------------------------------------------------------------------------
// Strategy: LRU
// 使用 GeneralQueue + "Promote" 技巧實作
// -----------------------------------------------------------------------------
class LRUEvictStrategy : public EvictStrategy {
private:
    GeneralQueue<logical_frame_t, frame_id_t> queue_;
    FrameAllocator allocator_;

public:
    explicit LRUEvictStrategy(size_t capacity) 
        : queue_(capacity / CXL_SSD_PAGE_SIZE),
          allocator_(capacity / CXL_SSD_PAGE_SIZE) {}

    frame_id_t access(logical_frame_t key) override {
        // 1. HIT: Key 存在
        if (queue_.Contains(key)) {
            // 取得舊的 frame_id
            frame_id_t fid = *queue_.Get(key);

            // 【LRU 關鍵動作】: 把資料移到最後面 (變為最新)
            queue_.Promote(key);

            return fid;
        }

        // 2. MISS: Key 不存在
        frame_id_t target_fid;

        if (!queue_.IsFull()) {
            // A. 未滿：直接分配
            target_fid = allocator_.allocate().value();
        } 
        else {
            // B. 已滿：LRU 的 Victim 也是最前面那個 (Head)
            // 先偷看 ID，然後讓 Insert 自動踢掉它
            auto victim_key = *queue_.PeekFront();
            target_fid = *queue_.Get(victim_key);
        }

        // 插入新資料 (變成 MRU)
        queue_.Insert(key, target_fid);
        
        return target_fid;
    }
};

// -----------------------------------------------------------------------------
// Strategy: LFRU (Least Frequent Recently Used)
// 使用 Set (紅黑樹) 進行排序 O(log N)
// -----------------------------------------------------------------------------
class LFRUEvictStrategy : public EvictStrategy {
private:
    struct LFRUNode {
        uint64_t cnt;         // Frequency
        uint64_t time;        // Recency
        logical_frame_t key;
        frame_id_t frame_id;

        // 排序：頻率小的在前，頻率相同則時間小的在前 (Victim 在 begin())
        bool operator<(const LFRUNode& other) const {
            if (cnt != other.cnt) return cnt < other.cnt;
            return time < other.time;
        }
    };

    std::set<LFRUNode> cache_set_;
    std::unordered_map<logical_frame_t, std::set<LFRUNode>::iterator> map_;
    FrameAllocator allocator_;
    uint64_t global_time_ = 0;

public:
    explicit LFRUEvictStrategy(size_t capacity) 
        : allocator_(capacity / CXL_SSD_PAGE_SIZE) {}

    frame_id_t access(logical_frame_t key) override {
        // 1. HIT
        if (auto it = map_.find(key); it != map_.end()) {
            LFRUNode node = *(it->second);
            
            // Remove old
            cache_set_.erase(it->second);
            map_.erase(it);

            // Update
            node.cnt++;
            node.time = ++global_time_;

            // Re-insert
            auto res = cache_set_.insert(node);
            map_[key] = res.first;
            
            return node.frame_id;
        }

        // 2. MISS
        frame_id_t target_fid;
        auto fid_opt = allocator_.allocate();

        if (fid_opt.has_value()) {
            target_fid = fid_opt.value();
        } else {
            // Evict: begin() is the victim
            auto victim_it = cache_set_.begin();
            target_fid = victim_it->frame_id; // Reuse ID
            
            map_.erase(victim_it->key);
            cache_set_.erase(victim_it);
        }

        // Insert new (cnt=1)
        LFRUNode new_node{1, ++global_time_, key, target_fid};
        auto res = cache_set_.insert(new_node);
        map_[key] = res.first;

        return target_fid;
    }
};

// -----------------------------------------------------------------------------
// TwoQ Strategy (使用原本的 GeneralQueue)
// -----------------------------------------------------------------------------
class TwoQEvictStrategy : public EvictStrategy {
private:
    // 定義方便的別名
    using QueueType = GeneralQueue<logical_frame_t, frame_id_t>;

    QueueType fifo_queue_; // A1_in (FIFO)
    QueueType lru_queue_;  // Am (LRU)
    FrameAllocator allocator_;

public:
    // 建構子：依照 TwoQ 邏輯分配容量 (這裡假設各一半，你可以自己改比例)
    explicit TwoQEvictStrategy(size_t capacity) 
        : fifo_queue_(capacity / 2 / CXL_SSD_PAGE_SIZE), 
          lru_queue_(capacity / 2 / CXL_SSD_PAGE_SIZE),
          allocator_(capacity / CXL_SSD_PAGE_SIZE) {}

    frame_id_t access(logical_frame_t key) override {
        // ---------------------------------------------------------
        // 1. 檢查 LRU Queue (Am)
        // ---------------------------------------------------------
        if (lru_queue_.Contains(key)) {
            // HIT on LRU: 需要把它移到最後面 (Move to MRU)
            
            // Step A: 拿值
            frame_id_t fid = *lru_queue_.Get(key);
            
            lru_queue_.Promote(key);

            return fid;
        }

        // ---------------------------------------------------------
        // 2. 檢查 FIFO Queue (A1_in)
        // ---------------------------------------------------------
        if (fifo_queue_.Contains(key)) {
            // HIT on FIFO: 晉升到 LRU (Promote)
            
            // Step A: 拿值並從 FIFO 移除
            frame_id_t fid = *fifo_queue_.Get(key);
            fifo_queue_.Remove(key);

            // Step B: 插入 LRU Queue
            InsertToLRU(key, fid);
            
            return fid;
        }

        // ---------------------------------------------------------
        // 3. MISS (都沒找到) -> 插入 FIFO Queue
        // ---------------------------------------------------------
        frame_id_t new_fid;
        auto alloc_res = allocator_.allocate();

        if (alloc_res.has_value()) {
            // 有閒置空間，直接用
            new_fid = alloc_res.value();
        } 
        else {
            // 空間全滿，必須踢人 (Evict)
            // 策略：優先踢 FIFO，FIFO 空了才踢 LRU
            if (fifo_queue_.PeekFront() != nullptr) {
                // 1. 取得受害者 ID
                logical_frame_t* vic_key = fifo_queue_.PeekFront();
                frame_id_t vic_fid = *fifo_queue_.Get(*vic_key);
                
                // 2. 踢掉
                fifo_queue_.PopFront();
                
                // 3. 重用它的 ID
                new_fid = vic_fid;
            } 
            else {
                // FIFO 是空的，只好踢 LRU
                assert(lru_queue_.PeekFront() != nullptr); // 總不可能兩個都空還 allocate 失敗
                
                logical_frame_t* vic_key = lru_queue_.PeekFront();
                frame_id_t vic_fid = *lru_queue_.Get(*vic_key);
                
                lru_queue_.PopFront();
                new_fid = vic_fid;
            }
        }

        // 插入新資料到 FIFO
        InsertToFIFO(key, new_fid);
        return new_fid;
    }

private:
    // 輔助函式：插入 LRU，並處理該 Queue 滿出來的情況
    void InsertToLRU(logical_frame_t key, frame_id_t fid) {
        // 嘗試插入。如果 lru_queue_ 滿了，它會自動回傳被踢掉的 victim
        auto victim = lru_queue_.Insert(key, fid);
        
        if (victim.has_value()) {
            // 如果有人被踢出來，記得把他的 ID 回收給 Allocator
            allocator_.deallocate(victim->second);
        }
    }

    // 輔助函式：插入 FIFO，並處理該 Queue 滿出來的情況
    void InsertToFIFO(logical_frame_t key, frame_id_t fid) {
        auto victim = fifo_queue_.Insert(key, fid);
        
        if (victim.has_value()) {
            allocator_.deallocate(victim->second);
        }
    }
};
} // namespace gem5