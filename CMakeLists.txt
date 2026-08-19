cmake_minimum_required(VERSION 3.22)
project(trading_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compile flags ─────────────────────────────────────────────────────────────

set(CMAKE_CXX_FLAGS_RELEASE
    "-O3 -march=native -mtune=native -DNDEBUG -funroll-loops")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO
    "-O2 -march=native -g -DNDEBUG")

add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-ignored-attributes       # silence mmap/mlock attribute warnings
    -fno-exceptions               # no exception overhead on hot path
    -fno-rtti                     # no RTTI overhead
)

# ── Sanitizer options ─────────────────────────────────────────────────────────

option(TSAN "Enable ThreadSanitizer" OFF)
if(TSAN)
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()

option(ASAN "Enable AddressSanitizer" OFF)
if(ASAN)
    add_compile_options(-fsanitize=address -g -O1)
    add_link_options(-fsanitize=address)
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────

find_package(Threads REQUIRED)

# ── Core engine library ───────────────────────────────────────────────────────

add_library(engine_core STATIC
    src/core/order_book.cpp
    src/core/matching_engine.cpp
    src/core/market_data.cpp
    src/core/execution_layer.cpp
    src/core/itch_parser.cpp
    src/util/logger.cpp
    src/util/allocator.cpp
)

target_include_directories(engine_core PUBLIC include)
target_link_libraries(engine_core PUBLIC Threads::Threads)

# ── Main executable ───────────────────────────────────────────────────────────

add_executable(trading_engine src/main.cpp)
target_link_libraries(trading_engine PRIVATE engine_core)

# ── Benchmarks ────────────────────────────────────────────────────────────────

add_executable(bench_latency bench/bench_latency.cpp)
target_link_libraries(bench_latency PRIVATE engine_core)

add_executable(bench_throughput bench/bench_throughput.cpp)
target_link_libraries(bench_throughput PRIVATE engine_core)

add_executable(bench_cache_miss bench/bench_cache_miss.cpp)
target_include_directories(bench_cache_miss PRIVATE include)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()

add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE engine_core)
add_test(NAME OrderBook COMMAND test_order_book)

add_executable(test_spsc tests/test_spsc.cpp)
target_link_libraries(test_spsc PRIVATE engine_core)
add_test(NAME SPSC COMMAND test_spsc)

add_executable(test_matching tests/test_matching.cpp)
target_link_libraries(test_matching PRIVATE engine_core)
add_test(NAME Matching COMMAND test_matching)

add_executable(test_allocator tests/test_allocator.cpp)
target_link_libraries(test_allocator PRIVATE engine_core)
add_test(NAME Allocator COMMAND test_allocator)

add_executable(test_microstructure tests/test_microstructure.cpp)
target_link_libraries(test_microstructure PRIVATE engine_core)
add_test(NAME Microstructure COMMAND test_microstructure)

add_executable(test_itch_parser tests/test_itch_parser.cpp)
target_link_libraries(test_itch_parser PRIVATE engine_core)
add_test(NAME ITCHParser COMMAND test_itch_parser)

# ── Simulation demo ───────────────────────────────────────────────────────────

add_executable(simulation_demo src/simulation_demo.cpp)
target_link_libraries(simulation_demo PRIVATE engine_core)
