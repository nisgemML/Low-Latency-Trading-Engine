#!/usr/bin/env bash
# scripts/profile.sh — CPU profiling via perf + flamegraph generation.
#
# Prerequisites:
#   sudo apt install linux-perf
#   git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph
#
# Usage:
#   sudo ./scripts/profile.sh                  # profile throughput bench, 10s
#   sudo ./scripts/profile.sh latency 30       # profile latency bench, 30s
#   sudo ./scripts/profile.sh --annotate       # also run perf annotate on hot syms

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$HOME/FlameGraph}"
OUT_DIR="$PROJECT_ROOT/perf_out"

TARGET="${1:-throughput}"
DURATION="${2:-10}"
ANNOTATE="${3:-}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info() { printf "${GREEN}[perf]${NC} %s\n" "$*"; }
warn() { printf "${YELLOW}[warn]${NC} %s\n" "$*"; }

# ── Sanity checks ─────────────────────────────────────────────────────────────

if [[ $EUID -ne 0 ]]; then
    warn "perf record typically requires root or CAP_PERFMON."
    warn "Re-running with sudo..."
    exec sudo -E "$0" "$@"
fi

if ! command -v perf &>/dev/null; then
    printf "${RED}[error]${NC} perf not found. Install: sudo apt install linux-perf\n"
    exit 1
fi

if [[ ! -d "$FLAMEGRAPH_DIR" ]]; then
    warn "FlameGraph not found at $FLAMEGRAPH_DIR"
    warn "Clone with: git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph"
    warn "Skipping flamegraph generation, will still capture perf data."
    HAS_FLAMEGRAPH=0
else
    HAS_FLAMEGRAPH=1
fi

# ── Build with frame pointers (needed for perf stack unwinding) ───────────────

info "Building with frame pointers for accurate stack unwinding..."
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer -g" \
    -Wno-dev -q
cmake --build "$BUILD_DIR" --parallel "$(nproc)" --target bench_latency bench_throughput

mkdir -p "$OUT_DIR"
PERF_DATA="$OUT_DIR/perf_${TARGET}.data"

# ── Select binary ─────────────────────────────────────────────────────────────

case "$TARGET" in
    throughput) BINARY="$BUILD_DIR/bench_throughput" ;;
    latency)    BINARY="$BUILD_DIR/bench_latency" ;;
    *)
        printf "${RED}[error]${NC} Unknown target: $TARGET (use 'throughput' or 'latency')\n"
        exit 1
        ;;
esac

# ── Pin to CPU 2 to reduce noise ─────────────────────────────────────────────

TASKSET_CMD=""
if command -v taskset &>/dev/null; then
    TASKSET_CMD="taskset -c 2"
    info "Pinning to CPU 2 via taskset"
fi

# ── Record ────────────────────────────────────────────────────────────────────

info "Recording $DURATION seconds of perf data for: $TARGET"
info "Output: $PERF_DATA"

# -F 999: sample at 999 Hz (avoids lockstep with 1kHz timer)
# -g: capture call graphs via frame pointers
# --call-graph fp: use frame pointer unwinding (faster than DWARF)
$TASKSET_CMD perf record \
    -F 999 \
    -g --call-graph fp \
    -o "$PERF_DATA" \
    --duration "$DURATION" \
    -- "$BINARY" &

PERF_PID=$!
info "perf PID: $PERF_PID  —  running for ${DURATION}s..."
wait "$PERF_PID" || true

# ── perf report (text summary) ────────────────────────────────────────────────

REPORT_FILE="$OUT_DIR/report_${TARGET}.txt"
info "Generating perf report → $REPORT_FILE"
perf report -i "$PERF_DATA" --stdio --no-children --sort=dso,symbol 2>/dev/null \
    | head -100 > "$REPORT_FILE"
cat "$REPORT_FILE"

# ── Flamegraph ────────────────────────────────────────────────────────────────

if [[ "$HAS_FLAMEGRAPH" -eq 1 ]]; then
    SVG_FILE="$OUT_DIR/flamegraph_${TARGET}.svg"
    info "Generating flamegraph → $SVG_FILE"

    perf script -i "$PERF_DATA" 2>/dev/null \
        | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" \
        | "$FLAMEGRAPH_DIR/flamegraph.pl" \
              --title "trading-engine ${TARGET} — $(date +%Y-%m-%d)" \
              --width 1600 \
              --colors hot \
        > "$SVG_FILE"

    info "Open in browser: file://$SVG_FILE"
fi

# ── perf annotate on hot functions ────────────────────────────────────────────

if [[ -n "$ANNOTATE" ]]; then
    ANNOTATE_FILE="$OUT_DIR/annotate_${TARGET}.txt"
    info "Running perf annotate → $ANNOTATE_FILE"
    perf annotate -i "$PERF_DATA" --stdio 2>/dev/null \
        | grep -A 50 "engine::OrderBook\|engine::SPSCQueue\|try_match\|add_order" \
        | head -200 > "$ANNOTATE_FILE"
    cat "$ANNOTATE_FILE"
fi

# ── Cache stats ───────────────────────────────────────────────────────────────

CACHE_FILE="$OUT_DIR/cache_${TARGET}.txt"
info "Measuring cache performance → $CACHE_FILE"

$TASKSET_CMD perf stat \
    -e cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,\
LLC-loads,LLC-load-misses,instructions,cycles \
    --duration "$DURATION" \
    -- "$BINARY" 2>&1 | tee "$CACHE_FILE" || true

info "All output in: $OUT_DIR/"
