rm -f main out.perf perf.data perf.data.old flamegraph.svg

clang++ -std=c++23 -O2 -g -fno-omit-frame-pointer -Werror -Wall -o main main.cpp

# Record stack samples (requires sudo or perf_event_paranoid relaxed)
sudo perf record -F 997 -g -- ./main

# Convert perf data to folded stacks
perf script > out.perf
./stackcollapse-perf.pl out.perf > out.folded

# Generate the flamegraph
./flamegraph.pl out.folded > flamegraph.svg
