# 🧪 單元測試快速參考

## ⚡ 最快開始方式

```bash
cd tests/unit_tests
make test
```

## 📋 所有命令

| 命令                 | 說明                   |
| -------------------- | ---------------------- |
| `make help`          | 顯示幫助信息           |
| `make build`         | 只編譯測試             |
| `make test`          | 編譯並運行所有測試     |
| `make test-verbose`  | 運行測試並顯示詳細輸出 |
| `make test-fifo`     | 只運行 FIFO Queue 測試 |
| `make test-strategy` | 只運行淘汰策略測試     |
| `make test-memory`   | 只運行 CxlMemory 測試  |
| `make clean`         | 清理編譯文件           |

## 📁 文件說明

| 文件                     | 行數 | 說明                      |
| ------------------------ | ---- | ------------------------- |
| `fifo_queue_test.cc`     | 182  | 13 個 FIFO Queue 測試用例 |
| `evict_strategy_test.cc` | 210  | 16 個淘汰策略測試用例     |
| `cxl_memory_test.cc`     | 196  | 7 個 CxlMemory 相關測試   |
| `CMakeLists.txt`         | 97   | CMake 配置                |
| `Makefile`               | 31   | 快速命令                  |
| `README.md`              | 200+ | 詳細文檔                  |

**總計：588 行測試代碼 + 配置**

## 🎯 測試覆蓋統計

```
FIFOQueue              13 tests  ✓
DirectEvictStrategy     3 tests  ✓
FIFOEvictStrategy       3 tests  ✓
LRUEvictStrategy        3 tests  ✓
LFRUEvictStrategy       1 tests  ✓
TwoQEvictStrategy       2 tests  ✓
Page Structure          6 tests  ✓
FrameAllocator          5 tests  ✓
─────────────────────────────────
總計                  36+ 測試  ✓
```

## 🔍 常用命令快速查找

### 編譯相關

```bash
make build              # 編譯所有測試
```

### 運行相關

```bash
make test               # 運行所有測試（推薦）
make test-verbose       # 詳細輸出
make test-fifo          # 只測 FIFO
make test-strategy      # 只測淘汰策略
make test-memory        # 只測 CxlMemory
```

### 清理相關

```bash
make clean              # 刪除 build 目錄
```

### 調試相關

```bash
./build/fifo_queue_test --gtest_filter=TestName
./build/fifo_queue_test --gtest_list_tests
```

## 💡 建議工作流

### 開發新功能時

```bash
# 1. 編寫新功能
# 2. 編寫對應測試
# 3. 運行測試
make test

# 4. 如果失敗，運行特定測試調試
make test-fifo
```

### 提交代碼前

```bash
# 確保所有測試通過
make clean
make test
```

### 性能調查時

```bash
# 運行特定測試並測量時間
time ./build/evict_strategy_test
```

## 🛠️ 自定義構建

### 啟用代碼覆蓋率

```bash
cd tests/unit_tests/build
cmake .. -DCMAKE_CXX_FLAGS="--coverage"
make
make run_tests
gcov *.cc
```

### 編譯優化

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 調試符號

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
make
```

## 📊 預期輸出範例

```
[100%] Built target cxl_memory_test
Test project /path/to/build
    Start 1: FifoQueueTest
1/3 Test #1: FifoQueueTest ..................... Passed    0.01 sec
    Start 2: EvictStrategyTest
2/3 Test #2: EvictStrategyTest ................ Passed    0.02 sec
    Start 3: CxlMemoryTest
3/3 Test #3: CxlMemoryTest .................... Passed    0.01 sec

100% tests passed, 0 tests failed out of 3
```

## 🚨 故障排除

| 問題             | 解決方案                                                            |
| ---------------- | ------------------------------------------------------------------- |
| cmake 命令未找到 | 安裝 cmake：`brew install cmake` (macOS)                            |
| 找不到頭文件     | 檢查 CMakeLists.txt 中的路徑是否正確                                |
| gtest 下載失敗   | 檢查網絡連接，重新運行 `make clean && make test`                    |
| 編譯錯誤         | 確保 C++17 編譯器：`make clean && cmake .. -DCMAKE_CXX_STANDARD=17` |

## 📖 更多信息

- 詳細文檔：見 `README.md`
- 配置詳情：見 `CMakeLists.txt`
- 安裝步驟：見 `SETUP_COMPLETE.md`

## ✨ 特色

- ✅ 36+ 單元測試用例
- ✅ Google Test (gtest) 框架
- ✅ 自動下載依賴，無需預裝
- ✅ 一鍵編譯運行（`make test`）
- ✅ 支持選擇性運行測試
- ✅ 支持代碼覆蓋率分析
- ✅ 支持 gdb 調試

---

**提示：** 首次運行時會下載並編譯 gtest，約 1-2 分鐘。之後會快速很多。
