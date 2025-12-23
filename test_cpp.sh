#!/bin/bash

set -uo pipefail

INPUT_PATTERN=${1:-"test/resources/samples/measurements-*.txt"}

if [ "$INPUT_PATTERN" = "-h" ]; then
  echo "Usage: ./test_cpp.sh [input file pattern]"
  echo "Default: test/resources/samples/measurements-*.txt"
  exit 1
fi

if [ -t 1 ]; then
  GREEN='\033[0;32m'
  RED='\033[0;31m'
  YELLOW='\033[0;33m'
  RESET='\033[0m'
else
  GREEN=""
  RED=""
  YELLOW=""
  RESET=""
fi

echo "Building C++ solution..."
cd cpp
clang++ -std=c++23 -O2 -Wall -o main main.cpp || exit 1
cd ..

total=0
passed=0
failed=0

for input_file in $INPUT_PATTERN; do
  expected_file="${input_file%.txt}.out"

  if [ ! -f "$expected_file" ]; then
    echo -e "${RED}SKIP${RESET} $input_file (no expected output)"
    continue
  fi

  total=$((total + 1))

  if ! actual=$(timeout 5 ./cpp/main "$input_file" 2>&1); then
    exit_code=$?
    failed=$((failed + 1))
    if [ $exit_code -eq 124 ]; then
      echo -e "${RED}TIMEOUT${RESET} $input_file"
    else
      echo -e "${RED}CRASH${RESET} $input_file (exit code: $exit_code)"
      echo "Error output:"
      echo "$actual"
    fi
    continue
  fi

  expected=$(cat "$expected_file")

  if [ "$actual" = "$expected" ]; then
    echo -e "${GREEN}PASS${RESET} $input_file"
    passed=$((passed + 1))
  else
    echo -e "${RED}FAIL${RESET} $input_file"
    failed=$((failed + 1))
    echo "Expected:"
    echo "$expected"
    echo "Actual:"
    echo "$actual"
  fi
done

echo ""
echo "========================"
echo -e "Total:  $total"
echo -e "${GREEN}Passed: $passed${RESET}"
echo -e "${RED}Failed: $failed${RESET}"
echo "========================"

if [ $failed -gt 0 ]; then
  exit 1
fi
