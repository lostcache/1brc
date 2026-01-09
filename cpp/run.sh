# Performance build (remove -fsanitize=undefined for max speed)
clang++ -std=c++23 -O3 -march=native -mtune=native \
  -DNDEBUG \
  -flto=auto \
  -ffast-math \
  -fno-exceptions \
  -fno-rtti \
  -funroll-loops \
  -fomit-frame-pointer \
  -fno-stack-protector \
  -fno-unwind-tables \
  -fno-asynchronous-unwind-tables \
  -finline-functions \
  -fno-plt \
  -fstrict-aliasing \
  -fno-semantic-interposition \
  -fvisibility=hidden \
  -fmerge-all-constants \
  -falign-functions=32 \
  -pthread \
  -Wall -Wextra \
  -o main main.cpp

# Debug build (uncomment to enable sanitizers)
# clang++ -std=c++23 -O2 -march=native -mtune=native \
#   -Wall -Wextra \
#   -fsanitize=undefined,address \
#   -g \
#   -o main main.cpp

# time ./main
