# CXL-SSD-Sim Unit Tests

本目錄包含 CXL-SSD-Sim 的單元測試，使用 Google Test (gtest) 框架。

## 測試範圍

### 1. **general_queue_test.cc**

測試 `GeneralQueue` 的核心功能：

- 插入和查詢
- 容量管理
- FIFO 淘汰順序
- Pop 和 Remove 操作

**測試用例：**

- `InsertAndContains` - 驗證插入後能找到元素
- `IsFull` - 驗證容量檢查
- `InsertEvictsFIFO` - 驗證 FIFO 淘汰順序
- `FIFOEvictionOrder` - 驗證多次淘汰的順序

### 2. **evict_strategy_test.cc**

測試各種淘汰策略的實現：

#### DirectEvictStrategy

- 直接映射的一致性
- 不同 key 映射到不同 frame_id

#### FIFOEvictStrategy

- Hit/Miss 行為
- FIFO 淘汰順序驗證

#### LRUEvictStrategy

- LRU 的 recency 更新
- Hit 時更新順序
- 淘汰最久未使用的

#### LFRUEvictStrategy

- 頻率追蹤
- 淘汰頻率最低的

#### TwoQEvictStrategy

- FIFO 到 LRU 的晉升機制
- 兩個隊列的交互

**測試用例：**

- `FIFOEvictionOrder` - 驗證 FIFO 淘汰順序
- `LRUEvictionOrder` - 驗證 LRU 淘汰最舊的
- `LRUHitUpdatesRecency` - 驗證訪問更新 recency
- `LFRUFrequencyTracking` - 驗證頻率追蹤
- `TwoQFIFOToLRUPromotion` - 驗證晉升機制

### 3. **cxl_memory_test.cc**

測試 CxlMemory 相關的結構和輔助類：

#### Page 結構

- 標誌位 (valid, dirty) 的設置和檢查
- Tag 的對齐和 cache hit 檢測

#### FrameAllocator

- 序列化分配
- 容量管理
- ID 回收和重用

## 快速開始

### 前置條件

- CMake 3.14 或更高
- C++ 17 編譯器 (g++, clang 等)

### 編譯和運行

```bash
# 進入測試目錄
cd tests/unit_tests

# 創建 build 目錄
mkdir -p build
cd build

# 配置 CMake
cmake ..

# 編譯所有測試
make

# 運行所有測試
make run_tests

# 或使用 ctest
ctest --output-on-failure
```

### 運行特定測試

```bash
# 運行 FIFO Queue 測試
./fifo_queue_test

# 運行 Evict Strategy 測試
./evict_strategy_test

# 運行 CxlMemory 測試
./cxl_memory_test

# 運行指定的測試用例
./fifo_queue_test --gtest_filter=FIFOQueueTest.InsertEvictsFIFO
```

### 顯示詳細信息

```bash
# 顯示所有測試用例
./fifo_queue_test --gtest_list_tests

# 運行測試並顯示詳細輸出
./fifo_queue_test --gtest_repeat=1 -v
```

## 測試覆蓋範圍

| 模塊             | 測試用例數 | 覆蓋率             |
| ---------------- | ---------- | ------------------ |
| FIFOQueue        | 13         | 核心操作           |
| Evict Strategies | 16         | 所有淘汰策略       |
| Page 結構        | 6          | 標誌位和 cache hit |
| FrameAllocator   | 5          | 分配和回收         |
| **總計**         | **40+**    | **~80%**           |

## 添加新的測試

### 範例：測試 LRU Cache 的一個特定場景

```cpp
TEST_F(LRUEvictStrategyTest, LRUAccessPatternOptimization) {
  LRUEvictStrategy strategy{5 * 1024}; // 5 pages

  // 模擬訪問模式
  std::vector<uint64_t> accesses = {1, 2, 3, 2, 1, 4, 5, 6};
  std::vector<frame_id_t> frame_ids;

  for (auto key : accesses) {
    frame_id_t fid = strategy.access(key);
    frame_ids.push_back(fid);
  }

  // 驗證預期的行為
  EXPECT_EQ(frame_ids.size(), accesses.size());
}
```

## 常見問題

### Q: 測試無法編譯，找不到頭文件？

**A:** 確保 CMakeLists.txt 中的 include 路徑正確指向源代碼目錄。

### Q: 如何調試測試？

**A:** 使用 gdb：

```bash
gdb ./fifo_queue_test
(gdb) break FIFOQueueTest::InsertEvictsFIFO
(gdb) run
```

### Q: 如何生成測試覆蓋率報告？

**A:** 使用 gcov 和 lcov：

```bash
cmake .. -DCMAKE_CXX_FLAGS="--coverage"
make
make run_tests
lcov --directory . --capture --output-file coverage.info
genhtml coverage.info --output-directory coverage_report
```

## 相關文件

- `CMakeLists.txt` - CMake 配置文件
- `fifo_queue_test.cc` - FIFO Queue 測試
- `evict_strategy_test.cc` - 淘汰策略測試
- `cxl_memory_test.cc` - CxlMemory 相關測試

## 貢獻

添加新測試時，請遵循：

1. 使用 `TEST_F` 或 `TEST` 宏
2. 清晰的測試名稱（描述性）
3. 每個測試只測試一個特性
4. 包含註釋解釋測試目的

## License

遵循 CXL-SSD-Sim 主項目的 license。
