#!/bin/bash
# 快速編譯和運行單元測試的腳本

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# 顏色定義
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}=== CXL-SSD-Sim Unit Tests ===${NC}"
echo

# 檢查依賴
if ! command -v cmake &> /dev/null; then
    echo -e "${RED}Error: cmake is not installed${NC}"
    exit 1
fi

if ! command -v make &> /dev/null; then
    echo -e "${RED}Error: make is not installed${NC}"
    exit 1
fi

# 運行測試
echo
echo -e "${YELLOW}Running tests...${NC}"
echo

if make run_tests; then
    echo
    echo -e "${GREEN}=== All tests passed! ===${NC}"
    exit 0
else
    echo
    echo -e "${RED}=== Some tests failed ===${NC}"
    exit 1
fi
