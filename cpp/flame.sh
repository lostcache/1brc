rm -f main out.stacks flamegraph.svg

clang++ -std=c++23 -O2 -g -fno-omit-frame-pointer -Werror -Wall -o main main.cpp

# Capture stack traces using DTrace (requires sudo)
sudo dtrace -c './main' \
    -o out.stacks \
    -n 'profile-997 /execname == "main"/ { @[ustack(100)] = count(); }'

# Generate the flamegraph
stackcollapse.pl out.csv | flamegraph.pl > flamegraph.svg