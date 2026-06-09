#!/system/bin/sh
set -e

WORK="${WORK:-/data/local/tmp/md}"
CPU="${CPU:-0}"
BASELINE_WARMUP="${BASELINE_WARMUP:-2000}"
BASELINE_ITERS="${BASELINE_ITERS:-20000}"
EMPTY_ITERS="${EMPTY_ITERS:-20000}"
PENDING_ITERS="${PENDING_ITERS:-200}"
PENDING_SIZE="${PENDING_SIZE:-33554432}"
STRICT_NEXT="${STRICT_NEXT:-1}"

mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true
cd "$WORK" || exit 1

for p in $(pidof memory_delegation_broker 2>/dev/null) \
         $(pidof scudo_shared_arena_test 2>/dev/null); do
  kill -TERM "$p" || true
done
sleep 1
for p in $(pidof memory_delegation_broker 2>/dev/null) \
         $(pidof scudo_shared_arena_test 2>/dev/null); do
  kill -KILL "$p" || true
done

echo "== baseline sched_yield =="
YIELD_BENCH_CPU="$CPU" \
YIELD_BENCH_WARMUP="$BASELINE_WARMUP" \
YIELD_BENCH_ITERS="$BASELINE_ITERS" \
YIELD_BENCH_STRICT_NEXT="$STRICT_NEXT" \
"$WORK/sched_yield_latency_bench"

rm -f broker.log
./memory_delegation_broker --trace >broker.log 2>&1 &
for i in $(seq 1 100); do
  grep -q "ready num_cores=" broker.log && break
  sleep 0.1
done
grep -q "ready num_cores=" broker.log || {
  echo "broker not ready"
  cat broker.log || true
  exit 1
}

echo "== active-empty yield =="
echo reset > /sys/kernel/debug/memory_delegation/switch_cycle_stats
echo 1 > /sys/kernel/debug/memory_delegation/switch_cycle_stats_enabled
SCUDO_SHARED_ARENA_FORCE=1 \
SCUDO_SHARED_ARENA_TRACE=0 \
SCUDO_SHARED_ARENA_TEST_CPU="$CPU" \
SCUDO_SHARED_ARENA_TEST_MODE=empty_yield \
SCUDO_SHARED_ARENA_TEST_EMPTY_ITERS="$EMPTY_ITERS" \
SCUDO_SHARED_ARENA_TEST_YIELD_STATS=1 \
SCUDO_SHARED_ARENA_TEST_STRICT_NEXT="$STRICT_NEXT" \
SCUDO_SHARED_ARENA_TEST_RESET_KERNEL_STATS_BEFORE_START=1 \
./scudo_shared_arena_test
echo 0 > /sys/kernel/debug/memory_delegation/switch_cycle_stats_enabled
cat /sys/kernel/debug/memory_delegation/switch_cycle_stats

echo "== pending-sync yield =="
echo reset > /sys/kernel/debug/memory_delegation/switch_cycle_stats
echo 1 > /sys/kernel/debug/memory_delegation/switch_cycle_stats_enabled
SCUDO_SHARED_ARENA_FORCE=1 \
SCUDO_SHARED_ARENA_TRACE=0 \
SCUDO_SHARED_ARENA_TEST_CPU="$CPU" \
SCUDO_SHARED_ARENA_TEST_MODE=pending_sync_yield \
SCUDO_SHARED_ARENA_TEST_PENDING_ITERS="$PENDING_ITERS" \
SCUDO_SHARED_ARENA_TEST_PENDING_SIZE="$PENDING_SIZE" \
SCUDO_SHARED_ARENA_TEST_STRICT_NEXT="$STRICT_NEXT" \
SCUDO_SHARED_ARENA_TEST_RESET_KERNEL_STATS_BEFORE_START=1 \
./scudo_shared_arena_test
echo 0 > /sys/kernel/debug/memory_delegation/switch_cycle_stats_enabled
cat /sys/kernel/debug/memory_delegation/switch_cycle_stats
