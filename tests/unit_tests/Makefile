.PHONY: help build clean test test-verbose test-queue test-strategy

help:
	@echo "=== CXL-SSD-Sim Unit Tests (Wrapper) ==="
	@echo ""
	@echo "Available targets:"
	@echo "  build          - Configure and Compile the project"
	@echo "  test           - Run all tests ONLY (No rebuild)"
	@echo "  test-verbose   - Run all tests with full logs (No rebuild)"
	@echo "  test-queue     - Run only GeneralQueue tests (No rebuild)"
	@echo "  test-strategy  - Run only EvictStrategy tests (No rebuild)"
	@echo "  clean          - Clean build artifacts"
	@echo "  rebuild        - Clean, Build, and then Test"
	@echo "  help           - Show this help message"

# 1. 建置 (Build)
# 確保 build 資料夾存在 -> 跑 cmake -> 跑 make
build:
	@mkdir -p build
	@cd build && cmake .. && make --no-print-directory -j`nproc`

# 2. 純測試 (Test Only)
# 這裡去掉了 'build' 依賴，符合你「不要再跑一次 build」的要求
# 使用 ctest --output-on-failure 若測試失敗會顯示詳細原因
test:
	@echo "Running tests (skipping build step)..."
	@cd build && ctest --output-on-failure

# 3. 詳細測試 (Verbose)
# 加上 -V 可以看到所有 cout 的輸出
test-verbose:
	@echo "Running verbose tests..."
	@cd build && ctest -V

# 4. 單獨跑 Queue 測試
test-queue:
	@echo "Running GeneralQueueTest..."
	@./build/general_queue_test

# 5. 單獨跑 Strategy 測試
test-strategy:
	@echo "Running EvictStrategyTest..."
	@./build/evict_strategy_test

# 6. 一鍵重練 (Rebuild + Test)
# 給你留一個方便的指令，如果你改了 code 想一次測到底
rebuild: clean build test

clean:
	@rm -rf build
	@echo "Cleaned build artifacts"

.DEFAULT_GOAL := help