.PHONY: help build clean test test-verbose test-queue test-strategy

help:
	@echo "=== CXL-SSD-Sim Unit Tests ==="
	@echo ""
	@echo "Available targets:"
	@echo "  build          - Build all tests"
	@echo "  test           - Build and run all tests"
	@echo "  test-verbose   - Run tests with verbose output"
	@echo "  test-queue     - Run only GeneralQueue tests"
	@echo "  test-strategy  - Run only eviction strategy tests"
	@echo "  clean          - Clean build artifacts"
	@echo "  help           - Show this help message"

build:
	@mkdir -p build
	@cd build && cmake .. && make

test: build
	@cd build && make run_tests

test-verbose: build
	@cd build && ctest --verbose --output-on-failure

test-queue: build
	@./build/general_queue_test

test-strategy: build
	@./build/evict_strategy_test

clean:
	@rm -rf build
	@echo "Cleaned build artifacts"

.DEFAULT_GOAL := help
