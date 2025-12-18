#include <gtest/gtest.h>
#include "dev/storage/evict_strategy.hh"

namespace gem5 {

#define CXL_SSD_PAGE_SIZE 4096

// ============================================================================
// Test: DirectEvictStrategy
// ============================================================================
class DirectEvictStrategyTest : public ::testing::Test {
protected:
  DirectEvictStrategy strategy{256 * CXL_SSD_PAGE_SIZE}; // 256 pages
};

TEST_F(DirectEvictStrategyTest, DirectMappingConsistency) {
  // 同一個 logical_frame 應該總是映射到同一個 frame_id
  uint64_t frame_id_1 = strategy.access(0x1000);
  uint64_t frame_id_2 = strategy.access(0x1000);
  EXPECT_EQ(frame_id_1, frame_id_2);
}

TEST_F(DirectEvictStrategyTest, DirectMappingDifferent) {
  // 不同的 logical_frame 應該映射到不同的 frame_id
  uint64_t frame_id_1 = strategy.access(0x1000);
  uint64_t frame_id_2 = strategy.access(0x2000);
  EXPECT_NE(frame_id_1, frame_id_2);
}

TEST_F(DirectEvictStrategyTest, DirectMappingMask) {
  // 驗證 Direct 映射的掩碼行為
  uint64_t capacity = 256; // 256 pages
  uint64_t frame_id = strategy.access(0x1000);
  EXPECT_LT(frame_id, capacity);
}

// ============================================================================
// Test: FIFOEvictStrategy
// ============================================================================
class FIFOEvictStrategyTest : public ::testing::Test {
protected:
  FIFOEvictStrategy strategy{1024 * CXL_SSD_PAGE_SIZE}; // 1024 pages (4MB / 4KB)
};

TEST_F(FIFOEvictStrategyTest, FIFOHitReturnsFrame) {
  // 插入一個 key，然後訪問它應該返回相同的 frame_id
  uint64_t frame_id_1 = strategy.access(0x1000);
  uint64_t frame_id_2 = strategy.access(0x1000);
  EXPECT_EQ(frame_id_1, frame_id_2);
}

TEST_F(FIFOEvictStrategyTest, FIFOMissAllocatesNewFrame) {
  // 新 key 應該分配新的 frame_id
  uint64_t frame_id_1 = strategy.access(0x1000);
  uint64_t frame_id_2 = strategy.access(0x2000);
  EXPECT_NE(frame_id_1, frame_id_2);
}

TEST_F(FIFOEvictStrategyTest, FIFOEvictionOrder) {
  // 測試 FIFO 淘汰順序：最舊的應該被淘汰
  
  // 創建一個小容量的 FIFO 以便測試
  FIFOEvictStrategy small_strategy{3 * CXL_SSD_PAGE_SIZE}; // 3 pages
  
  // 插入 3 個 key
  uint64_t fid_1 = small_strategy.access(0x1000);
  uint64_t fid_2 = small_strategy.access(0x2000);
  uint64_t fid_3 = small_strategy.access(0x3000);
  
  // 插入第 4 個，應該淘汰最舊的（0x1000）
  uint64_t fid_4 = small_strategy.access(0x4000);
  
  // fid_1 應該被重用
  EXPECT_EQ(fid_1, fid_4);
  
  // 驗證 0x1000 已被淘汰，再訪問應該得到新的 frame_id
  uint64_t fid_1_new = small_strategy.access(0x1000);
  EXPECT_NE(fid_1_new, fid_1);
}

// ============================================================================
// Test: LRUEvictStrategy
// ============================================================================
class LRUEvictStrategyTest : public ::testing::Test {
protected:
  LRUEvictStrategy strategy{1024 * CXL_SSD_PAGE_SIZE}; // 1024 pages
};

TEST_F(LRUEvictStrategyTest, LRUHitReturnsFrame) {
  // Hit 應該返回相同的 frame_id
  uint64_t fid_1 = strategy.access(0x1000);
  uint64_t fid_2 = strategy.access(0x1000);
  EXPECT_EQ(fid_1, fid_2);
}

TEST_F(LRUEvictStrategyTest, LRUHitUpdatesRecency) {
  // 測試 LRU 的 recency 更新
  
  LRUEvictStrategy small_strategy{3 * CXL_SSD_PAGE_SIZE}; // 3 pages
  
  // 插入 3 個 key
  uint64_t fid_1 = small_strategy.access(0x1000);
  uint64_t fid_2 = small_strategy.access(0x2000);
  uint64_t fid_3 = small_strategy.access(0x3000);
  
  // 重新訪問 0x1000，使其變成 MRU
  small_strategy.access(0x1000);
  
  // 插入第 4 個，應該淘汰 0x2000（最久未使用）而不是 0x1000
  uint64_t fid_4 = small_strategy.access(0x4000);
  
  // fid_2 應該被重用
  EXPECT_EQ(fid_2, fid_4);
  
  // 驗證 0x1000 仍然存在
  uint64_t fid_1_check = small_strategy.access(0x1000);
  EXPECT_EQ(fid_1, fid_1_check);
}

TEST_F(LRUEvictStrategyTest, LRUEvictionOrder) {
  // 測試 LRU 淘汰最久未使用的
  
  LRUEvictStrategy small_strategy{3 * CXL_SSD_PAGE_SIZE}; // 3 pages
  
  uint64_t fid_1 = small_strategy.access(0x1000);
  uint64_t fid_2 = small_strategy.access(0x2000);
  uint64_t fid_3 = small_strategy.access(0x3000);
  
  // 插入第 4 個，應該淘汰 0x1000（最久未使用）
  uint64_t fid_4 = small_strategy.access(0x4000);
  
  EXPECT_EQ(fid_1, fid_4);
}

// ============================================================================
// Test: LFRUEvictStrategy
// ============================================================================
class LFRUEvictStrategyTest : public ::testing::Test {
protected:
  LFRUEvictStrategy strategy{1024 * CXL_SSD_PAGE_SIZE}; // 1024 pages
};

TEST_F(LFRUEvictStrategyTest, LFRUFrequencyTracking) {
  // 訪問次數多的應該保留
  
  LFRUEvictStrategy small_strategy{3 * CXL_SSD_PAGE_SIZE}; // 3 pages
  
  // 插入 3 個 key
  small_strategy.access(0x1000);
  small_strategy.access(0x2000);
  small_strategy.access(0x3000);
  
  // 訪問 0x1000 和 0x2000 多次，增加其頻率
  for (int i = 0; i < 5; i++) {
    small_strategy.access(0x1000);
  }
  for (int i = 0; i < 3; i++) {
    small_strategy.access(0x2000);
  }
  
  // 插入第 4 個，應該淘汰 0x3000（頻率最低）
  uint64_t fid_4 = small_strategy.access(0x4000);
  
  // 驗證 0x1000 和 0x2000 仍然存在
  uint64_t fid_1_check = small_strategy.access(0x1000);
  uint64_t fid_2_check = small_strategy.access(0x2000);
  
  EXPECT_NE(fid_1_check, fid_4);
  EXPECT_NE(fid_2_check, fid_4);
}

// ============================================================================
// Test: TwoQEvictStrategy
// ============================================================================
class TwoQEvictStrategyTest : public ::testing::Test {
protected:
  TwoQEvictStrategy strategy{1024 * CXL_SSD_PAGE_SIZE}; // 1024 pages total (512 FIFO + 512 LRU)
};

TEST_F(TwoQEvictStrategyTest, TwoQHitReturnsFrame) {
  // Hit 應該返回相同的 frame_id
  uint64_t fid_1 = strategy.access(0x1000);
  uint64_t fid_2 = strategy.access(0x1000);
  EXPECT_EQ(fid_1, fid_2);
}

TEST_F(TwoQEvictStrategyTest, TwoQFIFOToLRUPromotion) {
  // FIFO 中的 key 訪問第二次時應該晉升到 LRU
  
  TwoQEvictStrategy small_strategy{6 * CXL_SSD_PAGE_SIZE}; // 6 pages (3 FIFO + 3 LRU)
  
  // 填滿 FIFO
  uint64_t fid_1 = small_strategy.access(0x1000);
  uint64_t fid_2 = small_strategy.access(0x2000);
  uint64_t fid_3 = small_strategy.access(0x3000);
  
  // 再次訪問第一個，應該晉升到 LRU
  uint64_t fid_1_check = small_strategy.access(0x1000);
  EXPECT_EQ(fid_1, fid_1_check);
  
  // 繼續添加直到需要淘汰
  // 此時 0x1000 在 LRU，應該被保護免於淘汰
  small_strategy.access(0x4000);
  small_strategy.access(0x5000);
  small_strategy.access(0x6000);
  
  // 0x1000 應該仍然存在於 LRU 中
  uint64_t fid_1_final = small_strategy.access(0x1000);
  EXPECT_EQ(fid_1, fid_1_final);
}

} // namespace gem5
