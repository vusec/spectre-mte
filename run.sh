clang poc.c -O0 -g -Iinclude -march=armv8.5-a+memtag -o poc
# big core
taskset -c 8 ./poc
