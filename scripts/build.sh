#!/usr/bin/env bash
# scripts/build.sh — Build, test, and optionally benchmark the trading engine.
#
# Usage:
#   ./scripts/build.sh              # Release build + tests
#   ./scripts/build.sh --tsan       # ThreadSanitizer build + concurrent tests
#   ./scripts/build.sh --bench      # Release build + benchmark
#   ./scripts/build.sh --clean      # Wipe build dir
#   ./scripts/build.sh --asan       # AddressSanitizer build + tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

info()    { printf "${GREEN}[build]${NC} %s\n" "$*"; }
warn()    { printf "${YELLOW}[warn] ${NC} %s\n" "$*"; }
err()     { printf "${RED}[error]${NC} %s\n" "$*" >&2; }

# ── Parse args ────────────────────────────────────────────────────────────────
TSAN=OFF; ASAN=OFF; BENCH=OFF; CLEAN=OFF
for arg in "$@"; do
    case "$arg" in
        --tsan)  TSAN=ON ;;
        --asan)  ASAN=ON ;;
        --bench) BENCH=ON ;;
        --clean) CLEAN=ON ;;
        *)       err "Unknown option: $arg"; exit 1 ;;
    esac
done

if [[ "$CLEAN" == "ON" ]]; then
    info "Cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    exit 0
fi

# ── Configure ─────────────────────────────────────────────────────────────────
BUILD_TYPE="Release"
[[ "$TSAN" == "ON" || "$ASAN" == "ON" ]] && BUILD_TYPE="RelWithDebInfo"

CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DTSAN="$TSAN"
    -DASAN="$ASAN"
    -G Ninja
)

info "Configuring (type=$BUILD_TYPE, TSAN=$TSAN, ASAN=$ASAN)..."
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}" -Wno-dev

# ── Build ─────────────────────────────────────────────────────────────────────
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
info "Building with $NPROC parallel jobs..."
cmake --build "$BUILD_DIR" --parallel "$NPROC"

# ── Test ──────────────────────────────────────────────────────────────────────
info "Running tests..."
cd "$BUILD_DIR"
ctest --output-on-failure --timeout 60
cd "$PROJECT_ROOT"

# ── Benchmark ─────────────────────────────────────────────────────────────────
if [[ "$BENCH" == "ON" ]]; then
    info "Running latency benchmark..."
    "$BUILD_DIR/bench_latency"

    info "Running throughput benchmark..."
    "$BUILD_DIR/bench_throughput"
fi

info "Done. ✓"
