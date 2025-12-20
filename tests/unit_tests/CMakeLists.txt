# ============================================================================
# CMakeLists.txt for CXL-SSD-Sim Unit Tests
# Focus: General Queue & Eviction Strategy
# ============================================================================

cmake_minimum_required(VERSION 3.14)
project(CXLSSDSimUnitTests CXX)

# 設定 C++ 標準
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")

# ----------------------------------------------------------------------------
# 1. 下載並配置 Google Test
# ----------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# 強制 GTest 使用共享 CRT，避免在 Windows/某些 Linux 環境下連結錯誤
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ----------------------------------------------------------------------------
# 2. 設置頭文件包含路徑 (Include Paths)
# ----------------------------------------------------------------------------
# 注意：如果移除 CXL Memory 架構後檔案位置有變，請確認這裡的路徑
set(INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/../../src
  ${CMAKE_SOURCE_DIR}/../../src/dev/storage
)

include_directories(${INCLUDE_DIRS})

message(STATUS "=== CXL-SSD-Sim Unit Tests Configuration ===")
message(STATUS "Source Dir:  ${CMAKE_SOURCE_DIR}")
message(STATUS "Includes:    ${INCLUDE_DIRS}")

enable_testing()

# ============================================================================
# 測試目標 1：General Queue (通用佇列測試)
# ============================================================================
add_executable(general_queue_test
  general_queue_test.cc
)
target_link_libraries(general_queue_test gtest gtest_main)
add_test(
  NAME GeneralQueueTest
  COMMAND general_queue_test
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# ============================================================================
# 測試目標 2：Evict Strategy (快取替換策略測試)
# ============================================================================
add_executable(evict_strategy_test
  evict_strategy_test.cc
)
target_link_libraries(evict_strategy_test gtest gtest_main)
add_test(
  NAME EvictStrategyTest
  COMMAND evict_strategy_test
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# ============================================================================
# 輔助命令：一鍵運行所有測試
# ============================================================================
add_custom_target(run_tests
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  DEPENDS general_queue_test evict_strategy_test
)

# ============================================================================
# 輔助命令：顯示幫助信息
# ============================================================================
add_custom_target(test_help
  COMMAND ${CMAKE_COMMAND} -E echo "========================================"
  COMMAND ${CMAKE_COMMAND} -E echo "   CXL-SSD-Sim Unit Test Environment"
  COMMAND ${CMAKE_COMMAND} -E echo "========================================"
  COMMAND ${CMAKE_COMMAND} -E echo "Available Targets:"
  COMMAND ${CMAKE_COMMAND} -E echo "  1. general_queue_test  : Run GeneralQueue logic tests"
  COMMAND ${CMAKE_COMMAND} -E echo "  2. evict_strategy_test : Run Eviction Strategy logic tests"
  COMMAND ${CMAKE_COMMAND} -E echo "  3. run_tests           : Run ALL above tests"
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "Usage:"
  COMMAND ${CMAKE_COMMAND} -E echo "  cmake .. && make run_tests"
  COMMAND ${CMAKE_COMMAND} -E echo "========================================"
)