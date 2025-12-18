#include <gtest/gtest.h>
#include "dev/storage/general_queue.hh"

namespace gem5 {

// ============================================================================
// Test: GeneralQueue Basic Operations
// ============================================================================
class GeneralQueueTest : public ::testing::Test {
protected:
  struct TestPayload {
    uint64_t value;
    TestPayload() : value(0) {}
    TestPayload(uint64_t v) : value(v) {}
  };
  
  GeneralQueue<uint64_t, TestPayload> queue{10}; // 10 elements
};

TEST_F(GeneralQueueTest, InsertAndContains) {
  // 插入後應該能找到
  TestPayload payload(42);
  queue.Insert(1, payload);
  
  EXPECT_TRUE(queue.Contains(1));
  EXPECT_FALSE(queue.Contains(2));
}

TEST_F(GeneralQueueTest, GetPayload) {
  // 應該能正確取得 payload
  TestPayload original(42);
  queue.Insert(1, original);
  
  auto* retrieved = queue.Get(1);
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->value, 42);
}

TEST_F(GeneralQueueTest, InsertReturnsSameKeyAsVictim) {
  // 包含相同的 key 時應該返回 nullopt
  TestPayload payload1(1);
  auto result1 = queue.Insert(1, payload1);
  
  TestPayload payload2(2);
  auto result2 = queue.Insert(1, payload2); // 同一個 key
  
  EXPECT_FALSE(result1.has_value());
  EXPECT_FALSE(result2.has_value()); // 應該返回 nullopt，不重複插入
}

TEST_F(GeneralQueueTest, IsFull) {
  // 測試容量檢查
  EXPECT_FALSE(queue.IsFull());
  
  // 填滿隊列
  for (int i = 0; i < 10; i++) {
    TestPayload payload(i);
    queue.Insert(i, payload);
  }
  
  EXPECT_TRUE(queue.IsFull());
}

TEST_F(GeneralQueueTest, InsertEvictsFIFO) {
  // 滿容量時應該淘汰最舊的（FIFO）
  GeneralQueue<uint64_t, TestPayload> small_queue{3};
  
  // 插入 3 個元素
  TestPayload p1(1), p2(2), p3(3);
  small_queue.Insert(1, p1);
  small_queue.Insert(2, p2);
  small_queue.Insert(3, p3);
  
  EXPECT_TRUE(small_queue.IsFull());
  
  // 插入第 4 個，應該淘汰 key=1
  TestPayload p4(4);
  auto victim = small_queue.Insert(4, p4);
  
  EXPECT_TRUE(victim.has_value());
  EXPECT_EQ(victim->first, 1); // 被淘汰的 key
  EXPECT_EQ(victim->second.value, 1); // 被淘汰的 payload
  
  // 驗證新 key 已插入
  EXPECT_TRUE(small_queue.Contains(4));
  EXPECT_FALSE(small_queue.Contains(1));
}

TEST_F(GeneralQueueTest, PopFront) {
  // PopFront 應該移除最舊的元素
  TestPayload p1(1), p2(2), p3(3);
  queue.Insert(1, p1);
  queue.Insert(2, p2);
  queue.Insert(3, p3);
  
  EXPECT_TRUE(queue.Contains(1));
  
  queue.PopFront();
  
  EXPECT_FALSE(queue.Contains(1));
  EXPECT_TRUE(queue.Contains(2));
  EXPECT_TRUE(queue.Contains(3));
}

TEST_F(GeneralQueueTest, Remove) {
  // Remove 應該移除指定的元素
  TestPayload p1(1), p2(2), p3(3);
  queue.Insert(1, p1);
  queue.Insert(2, p2);
  queue.Insert(3, p3);
  
  queue.Remove(2);
  
  EXPECT_TRUE(queue.Contains(1));
  EXPECT_FALSE(queue.Contains(2));
  EXPECT_TRUE(queue.Contains(3));
}

TEST_F(GeneralQueueTest, RemoveNonexistent) {
  // 移除不存在的元素不應該拋出異常
  EXPECT_NO_THROW({
    queue.Remove(999);
  });
}

TEST_F(GeneralQueueTest, PopFrontEmpty) {
  // 空隊列上 PopFront 不應該拋出異常
  EXPECT_NO_THROW({
    queue.PopFront();
  });
}

// ============================================================================
// Test: GeneralQueue FIFO Order
// ============================================================================
class GeneralQueueOrderTest : public ::testing::Test {
protected:
  GeneralQueue<std::string, int> queue{5};
};

TEST_F(GeneralQueueOrderTest, FIFOEvictionOrder) {
  // 驗證 FIFO 順序
  queue.Insert("a", 1);
  queue.Insert("b", 2);
  queue.Insert("c", 3);
  queue.Insert("d", 4);
  queue.Insert("e", 5);
  
  EXPECT_TRUE(queue.IsFull());
  
  // 插入第 6 個，應該淘汰 "a"
  auto victim = queue.Insert("f", 6);
  EXPECT_EQ(victim->first, "a");
}

TEST_F(GeneralQueueOrderTest, MultipleEvictions) {
  // 連續淘汰
  queue.Insert("a", 1);
  queue.Insert("b", 2);
  queue.Insert("c", 3);
  queue.Insert("d", 4);
  queue.Insert("e", 5);
  
  // 淘汰 "a"，插入 "f"
  auto victim1 = queue.Insert("f", 6);
  EXPECT_EQ(victim1->first, "a");
  
  // 淘汰 "b"，插入 "g"
  auto victim2 = queue.Insert("g", 7);
  EXPECT_EQ(victim2->first, "b");
  
  // 驗證狀態
  EXPECT_FALSE(queue.Contains("a"));
  EXPECT_FALSE(queue.Contains("b"));
  EXPECT_TRUE(queue.Contains("c"));
  EXPECT_TRUE(queue.Contains("d"));
  EXPECT_TRUE(queue.Contains("e"));
  EXPECT_TRUE(queue.Contains("f"));
  EXPECT_TRUE(queue.Contains("g"));
}

} // namespace gem5
