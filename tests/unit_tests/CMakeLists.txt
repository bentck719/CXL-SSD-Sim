# 簡化版 CMakeLists.txt，用於本地測試開發
# 適合在開發環境中快速編譯和運行單元測試

cmake_minimum_required(VERSION 3.14)
project(CXLSSDSimUnitTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")

# 下載並配置 Google Test
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# 設置頭文件包含路徑
set(INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/../../src
  ${CMAKE_SOURCE_DIR}/../../src/dev/storage
)

include_directories(${INCLUDE_DIRS})
include_directories(../../src)

message(STATUS "Current Source Dir: ${CMAKE_SOURCE_DIR}")
message(STATUS "Include Dirs: ${INCLUDE_DIRS}")

# 啟用測試
enable_testing()

# ============================================================================
# 測試目標：General Queue
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
# 測試目標：Evict Strategy
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
# 自定義命令：運行所有測試
# ============================================================================
add_custom_target(run_tests
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  DEPENDS general_queue_test evict_strategy_test
)

# ============================================================================
# 自定義命令：顯示幫助信息
# ============================================================================
add_custom_target(test_help
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "=== Unit Tests for CXL-SSD-Sim ==="
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "Available targets:"
  COMMAND ${CMAKE_COMMAND} -E echo "  - general_queue_test:      Run GeneralQueue tests"
  COMMAND ${CMAKE_COMMAND} -E echo "  - evict_strategy_test:     Run eviction strategy tests"
  COMMAND ${CMAKE_COMMAND} -E echo "  - run_tests:               Run all tests"
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "Build commands:"
  COMMAND ${CMAKE_COMMAND} -E echo "  mkdir -p build"
  COMMAND ${CMAKE_COMMAND} -E echo "  cd build"
  COMMAND ${CMAKE_COMMAND} -E echo "  cmake .."
  COMMAND ${CMAKE_COMMAND} -E echo "  make run_tests"
  COMMAND ${CMAKE_COMMAND} -E echo ""
)
