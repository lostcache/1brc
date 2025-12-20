#!/bin/bash

set -euo pipefail

INPUT_PATTERN=${1:-"test/resources/samples/measurements-*.txt"}

if [ "$INPUT_PATTERN" = "-h" ]; then
  echo "Usage: ./test_cpp.sh [input file pattern]"
  echo "Default: test/resources/samples/measurements-*.txt"
  exit 1
fi

if [ -t 1 ]; then
  GREEN='\033[0;32m'
  RED='\033[0;31m'
  RESET='\033[0m'
else
  GREEN=""
  RED=""
  RESET=""
fi

echo "Building C++ solution..."
cd cpp
clang++ -std=c++23 -O2 -Wall -o main main.cpp || exit 1
cd ..

for input_file in $INPUT_PATTERN; do
  expected_file="${input_file%.txt}.out"

  if [ ! -f "$expected_file" ]; then
    echo -e "${RED}SKIP${RESET} $input_file (no expected output)"
    continue
  fi

  actual=$(./cpp/main "$input_file")
  expected=$(cat "$expected_file")

  if [ "$actual" = "$expected" ]; then
    echo -e "${GREEN}PASS${RESET} $input_file"
  else
    echo -e "${RED}FAIL${RESET} $input_file"
    echo "Expected:"
    echo "$expected"
    echo "Actual:"
    echo "$actual"
  fi
done
