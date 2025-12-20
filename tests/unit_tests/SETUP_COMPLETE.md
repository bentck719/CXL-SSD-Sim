# 單元測試安裝完成

## 📦 已創建的文件結構

```
tests/unit_tests/
├── CMakeLists.txt              # CMake 配置文件
├── Makefile                    # 快速編譯/測試 Makefile
├── build_and_test.sh           # 一鍵編譯運行腳本
├── README.md                   # 詳細文檔
├── cxl_memory_test.cc          # CxlMemory 測試 (Page, FrameAllocator)
├── evict_strategy_test.cc      # 淘汰策略測試 (Direct, FIFO, LRU, LFRU, TwoQ)
└── fifo_queue_test.cc          # FIFO Queue 測試
```

## 🚀 快速開始

### 方式 1：使用 Makefile（推薦）

```bash
cd tests/unit_tests

# 查看所有可用命令
make help

# 編譯並運行所有測試
make test

# 運行特定測試
make test-fifo
make test-strategy
make test-memory

# 清理
make clean
```

### 方式 2：使用腳本

```bash
cd tests/unit_tests
./build_and_test.sh
```

### 方式 3：手動 CMake

```bash
cd tests/unit_tests
mkdir -p build
cd build
cmake ..
make
make run_tests
```

## 📊 測試覆蓋

| 模塊                    | 測試數  | 說明                        |
| ----------------------- | ------- | --------------------------- |
| **FIFOQueue**           | 13      | 插入、查詢、淘汰、FIFO 順序 |
| **DirectEvictStrategy** | 3       | 直接映射一致性              |
| **FIFOEvictStrategy**   | 3       | FIFO 淘汰順序               |
| **LRUEvictStrategy**    | 3       | LRU recency 更新和淘汰      |
| **LFRUEvictStrategy**   | 1       | 頻率追蹤                    |
| **TwoQEvictStrategy**   | 2       | FIFO→LRU 晉升               |
| **Page 結構**           | 6       | 標誌位、Tag、Cache Hit      |
| **FrameAllocator**      | 5       | 分配、回收、重用            |
| **總計**                | **36+** | -                           |

## ✅ 測試用例概覽

### FIFO Queue 測試

- ✓ `InsertAndContains` - 插入和查詢
- ✓ `IsFull` - 容量檢查
- ✓ `InsertEvictsFIFO` - FIFO 淘汰
- ✓ `PopFront` / `Remove` - 刪除操作
- ✓ `FIFOEvictionOrder` - 淘汰順序驗證

### 淘汰策略測試

- ✓ **Direct** - 一致性映射
- ✓ **FIFO** - 先進先出淘汰
- ✓ **LRU** - 最久未使用淘汰 + Recency 更新
- ✓ **LFRU** - 頻率追蹤 + 淘汰
- ✓ **TwoQ** - FIFO→LRU 晉升機制

### CxlMemory 相關測試

- ✓ `PageFlagsInitialization` - Page 標誌位
- ✓ `CacheHit` - Cache Hit 檢測
- ✓ `FrameAllocatorDeallocateAndReuse` - ID 回收重用

## 🔧 配置選項

### 啟用代碼覆蓋率

```bash
cd tests/unit_tests/build
cmake .. -DCMAKE_CXX_FLAGS="--coverage"
make
make run_tests
gcov *.cc
```

### 啟用 verbose 輸出

```bash
make test-verbose
```

## 📝 添加新測試

### 示例：為新淘汰策略添加測試

```cpp
class MyNewStrategyTest : public ::testing::Test {
protected:
  MyNewStrategy strategy{4096};
};

TEST_F(MyNewStrategyTest, TestFeature) {
  // 安排
  uint64_t frame_id = strategy.access(0x1000);

  // 執行
  uint64_t result = strategy.access(0x1000);

  // 斷言
  EXPECT_EQ(frame_id, result);
}
```

## 🐛 調試技巧

### 運行單個測試用例

```bash
cd build
./fifo_queue_test --gtest_filter=FIFOQueueTest.InsertEvictsFIFO
```

### 列出所有測試

```bash
cd build
./fifo_queue_test --gtest_list_tests
```

### 使用 gdb 調試

```bash
cd build
gdb ./fifo_queue_test
(gdb) break FIFOQueueTest::InsertEvictsFIFO
(gdb) run
```

## 🎯 下一步

1. **擴展測試覆蓋率**

   - 為 `general_queue.hh` 添加測試（如果有的話）
   - 添加集成測試

2. **性能測試**

   - 基準測試各淘汰策略的性能
   - 測試大規模數據集

3. **壓力測試**
   - 模擬高負載場景
   - 併發訪問測試

## 📚 文檔

- `README.md` - 詳細的測試文檔
- `CMakeLists.txt` - CMake 配置說明
- 各測試文件中的註釋

## 🤝 注意事項

1. 測試使用 Google Test (gtest) 自動下載，無需預先安裝
2. C++ 標準要求：C++17 或更高
3. CMake 要求：3.14 或更高
4. 首次編譯時會自動下載 gtest，可能需要網絡連接

## ❓ 常見問題

**Q: 為什麼第一次編譯很慢？**
A: gtest 第一次下載和編譯需要時間。之後會使用緩存。

**Q: 如何在 CI/CD 中運行測試？**
A: 使用 `make test` 或 `ctest` 命令，返回碼 0 表示成功。

**Q: 能否跳過某些測試？**
A: 使用 `--gtest_filter=-TestName` 排除特定測試。

---

祝測試順利！🎉
