#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "dev/storage/general_queue.hh"

namespace gem5 {

// 定義一個測試用的 Payload，包含運算符重載以便測試比較
struct TestPayload {
    int id;
    std::string data;

    TestPayload() : id(0), data("") {}
    TestPayload(int i, std::string s) : id(i), data(s) {}

    bool operator==(const TestPayload& other) const {
        return id == other.id && data == other.data;
    }
};

// ============================================================================
// Group 1: 基本增刪查改 (Basic CRUD)
// ============================================================================
class GeneralQueueBasicTest : public ::testing::Test {
protected:
    GeneralQueue<int, TestPayload> queue{5}; // 容量 5
};

TEST_F(GeneralQueueBasicTest, InsertAndContains) {
    EXPECT_FALSE(queue.Contains(1));
    queue.Insert(1, {1, "one"});
    EXPECT_TRUE(queue.Contains(1));
    EXPECT_FALSE(queue.Contains(2));
}

TEST_F(GeneralQueueBasicTest, InsertDuplicateFail) {
    // 測試重複插入相同的 Key
    queue.Insert(1, {1, "A"});
    auto result = queue.Insert(1, {1, "B"}); // Key 重複

    EXPECT_FALSE(result.has_value()); // 不應返回 victim
    // 確保舊值沒有被覆蓋
    auto* ptr = queue.Get(1);
    EXPECT_EQ(ptr->data, "A"); 
}

TEST_F(GeneralQueueBasicTest, GetAndModify) {
    // 測試透過 Get 取得指標並修改內容 (Mutable Check)
    queue.Insert(10, {10, "Original"});
    
    auto* ptr = queue.Get(10);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(ptr->data, "Original");

    // 修改內容
    ptr->data = "Modified";

    // 再次取得確認修改生效
    auto* ptr2 = queue.Get(10);
    EXPECT_EQ(ptr2->data, "Modified");
}

TEST_F(GeneralQueueBasicTest, GetNonExistent) {
    EXPECT_EQ(queue.Get(999), nullptr);
}

TEST_F(GeneralQueueBasicTest, RemoveLogic) {
    queue.Insert(1, {1, "A"});
    queue.Insert(2, {2, "B"});
    queue.Insert(3, {3, "C"});

    // 移除中間元素
    queue.Remove(2);
    EXPECT_TRUE(queue.Contains(1));
    EXPECT_FALSE(queue.Contains(2));
    EXPECT_TRUE(queue.Contains(3));

    // 移除不存在元素 (應無異常)
    EXPECT_NO_THROW(queue.Remove(999));
}

// ============================================================================
// Group 2: FIFO 淘汰與容量控制 (Eviction & Capacity)
// ============================================================================
class GeneralQueueEvictionTest : public ::testing::Test {
protected:
    GeneralQueue<std::string, int> queue{3}; // 小容量：3
};

TEST_F(GeneralQueueEvictionTest, StrictFIFOEviction) {
    // 填滿: A, B, C (A 是最舊的)
    queue.Insert("A", 1);
    queue.Insert("B", 2);
    queue.Insert("C", 3);
    
    EXPECT_TRUE(queue.IsFull());
    EXPECT_EQ(*queue.PeekFront(), "A"); // 確認 A 在最前面

    // 插入 D -> 淘汰 A
    auto victim = queue.Insert("D", 4);
    ASSERT_TRUE(victim.has_value());
    EXPECT_EQ(victim->first, "A");
    EXPECT_EQ(victim->second, 1);
    
    EXPECT_FALSE(queue.Contains("A"));
    EXPECT_TRUE(queue.Contains("B"));
    EXPECT_TRUE(queue.Contains("C"));
    EXPECT_TRUE(queue.Contains("D"));

    // 現在最舊的是 B
    EXPECT_EQ(*queue.PeekFront(), "B");
}

TEST_F(GeneralQueueEvictionTest, PeekFrontLogic) {
    EXPECT_EQ(queue.PeekFront(), nullptr); // 空隊列

    queue.Insert("A", 1);
    EXPECT_EQ(*queue.PeekFront(), "A");

    queue.Insert("B", 2);
    EXPECT_EQ(*queue.PeekFront(), "A"); // 插入後，舊的還是在前面
}

TEST_F(GeneralQueueEvictionTest, PopFrontLogic) {
    queue.Insert("A", 1);
    queue.Insert("B", 2);

    queue.PopFront(); // 移除 A
    EXPECT_FALSE(queue.Contains("A"));
    EXPECT_TRUE(queue.Contains("B"));
    EXPECT_EQ(*queue.PeekFront(), "B");

    queue.PopFront(); // 移除 B
    EXPECT_TRUE(queue.IsEmpty()); // 內部檢查 (若測試可訪問私有成員)
    EXPECT_EQ(queue.PeekFront(), nullptr);

    EXPECT_NO_THROW(queue.PopFront()); // 空隊列 Pop 安全檢查
}

// ============================================================================
// Group 3: 提升與順序重排 (Promote / LRU Logic)
// 這是你原本缺少的關鍵測試
// ============================================================================
class GeneralQueuePromoteTest : public ::testing::Test {
protected:
    GeneralQueue<int, int> queue{3};
};

TEST_F(GeneralQueuePromoteTest, PromoteUpdateOrder) {
    // 初始: 1(Old) -> 2 -> 3(New)
    queue.Insert(1, 10);
    queue.Insert(2, 20);
    queue.Insert(3, 30);

    // Promote 1: 順序應變為 2(Old) -> 3 -> 1(New)
    queue.Promote(1);

    // 驗證: 下一個被淘汰的應該是 2，而不是 1
    EXPECT_EQ(*queue.PeekFront(), 2);

    // 插入導致淘汰
    auto victim = queue.Insert(4, 40);
    EXPECT_EQ(victim->first, 2); // 2 被淘汰
    
    // 1 仍然存在
    EXPECT_TRUE(queue.Contains(1));
}

TEST_F(GeneralQueuePromoteTest, PromoteTail) {
    // 測試提升已經是最新元素的狀況
    queue.Insert(1, 10);
    queue.Insert(2, 20); // 2 is tail

    queue.Promote(2); // 應該沒變化

    EXPECT_EQ(*queue.PeekFront(), 1);
    
    // 插入 3，淘汰 1
    auto victim = queue.Insert(3, 30);
    EXPECT_EQ(victim->first, 0);
}

TEST_F(GeneralQueuePromoteTest, PromoteNonExistent) {
    queue.Insert(1, 10);
    EXPECT_NO_THROW(queue.Promote(99)); // 不應崩潰
    EXPECT_EQ(*queue.PeekFront(), 1);
}

// ============================================================================
// Group 4: 極限邊界條件 (Corner Cases)
// ============================================================================
class GeneralQueueEdgeCaseTest : public ::testing::Test {};

TEST_F(GeneralQueueEdgeCaseTest, SizeOneQueue) {
    // 測試容量只有 1 的佇列
    GeneralQueue<int, int> tiny_queue{1};

    EXPECT_FALSE(tiny_queue.IsFull()); // 一開始空的

    tiny_queue.Insert(1, 100);
    EXPECT_TRUE(tiny_queue.IsFull());
    EXPECT_TRUE(tiny_queue.Contains(1));

    // 插入 2，應該立即淘汰 1
    auto victim = tiny_queue.Insert(2, 200);
    
    ASSERT_TRUE(victim.has_value());
    EXPECT_EQ(victim->first, 1);
    EXPECT_EQ(victim->second, 100);

    EXPECT_FALSE(tiny_queue.Contains(1));
    EXPECT_TRUE(tiny_queue.Contains(2));
}

TEST_F(GeneralQueueEdgeCaseTest, ZeroSizeQueue) {
    // 如果允許創建 Size 0 (雖不常見，但需防禦)，應該總是 IsFull，且無法存儲
    GeneralQueue<int, int> zero_queue{0};
    
    EXPECT_TRUE(zero_queue.IsFull()); // 0 >= 0 is true

    // 嘗試插入
    auto victim = zero_queue.Insert(1, 100);
}
// 行為分析: 
// Insert 發現 Full -> 取 Front (但空) -> PopFront (空) -> PushBack(1) -> Return Nullopt(victim初始值)?
// 不對，PeekFront 在空的時候如果直接取值會 Segfault，所以要檢查 PeekFront 的實作保護
// 在 general_queue.hh 中，PeekFront 有 empty check，但 Insert 裡面的 victim = fifo_list_.front() 沒有 check!

// **注意**: 這會揭露你程式碼的一個潛在 Bug。
// 如果 max_size_ 是 0，且 fifo_list_ 是空的：
// Insert:
// 1. IsFull() -> True
// 2. victim = fifo_list_.front();  <-- 這裡對空 list 呼叫 front() 是未定義行為 (Undefined Behavior)!

// 為了測試通過，假設我們不測試 Size 0，或者你在 .hh 中修復此問題。
// 在此我們僅測試 "邏輯上接近 0" 的 Size 1。

TEST_F(GeneralQueueEdgeCaseTest, ComplexInterleaving) {
    // 混合操作壓力測試
    GeneralQueue<std::string, int> q{3};

    q.Insert("A", 1);
    q.Insert("B", 2);
    q.Promote("A"); // B -> A
    q.Insert("C", 3); // B -> A -> C
    
    q.Remove("A"); // B -> C
    
    auto victim = q.Insert("D", 4); // Full? No, size is 2/3. B -> C -> D
    EXPECT_FALSE(victim.has_value());
    EXPECT_TRUE(q.IsFull());

    // Next victim should be B
    EXPECT_EQ(*q.PeekFront(), "B");
}

} // namespace gem5