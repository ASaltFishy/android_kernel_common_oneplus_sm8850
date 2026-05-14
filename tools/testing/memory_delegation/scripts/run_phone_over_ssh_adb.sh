#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Run memory_delegation userspace tests on a physical phone via:
#   host -> ssh jump box -> adb -> phone
#
# Default assumptions:
# - The jump box has working `adb` access to the phone.
# - The phone can execute the pushed test binary (aarch64).
#
# Usage:
#   common/tools/testing/memory_delegation/scripts/run_phone_over_ssh_adb.sh
#
# Knobs:
#   JUMP_HOST=lrc@192.168.60.205   (ssh target)
#   ADB_SERIAL=...                 (optional: specific device)
#   PHONE_DIR=/data/local/tmp/md   (remote dir on phone)
#   USE_SU=1                       (run test under `su -c ...`)
#   BUILD=1                        (build test binary before running)
#   BUILD_MODE=android             (Makefile MODE when BUILD=1)
#   ANDROID_NDK=...                (NDK path when BUILD_MODE=android)
#   AARCH64_CXX=...                (toolchain override when BUILD_MODE=linux)
#   START_BROKER=1                 (start memory_delegation_broker before test)
#   KILL_STALE_BROKER=1            (kill prior broker/test processes first)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"          # memory_delegation/
TESTS_DIR="${BASE_DIR}/tests"
LOCAL_BIN="${LOCAL_BIN:-${TESTS_DIR}/out/scudo_shared_arena_test}"
LOCAL_BROKER="${LOCAL_BROKER:-${TESTS_DIR}/out/memory_delegation_broker}"

JUMP_HOST="${JUMP_HOST:-lrc@192.168.61.4}"
ADB_BIN="${ADB_BIN:-/opt/homebrew/bin/adb}"
ADB_SERIAL="${ADB_SERIAL:-}"
PHONE_DIR="${PHONE_DIR:-/data/local/tmp/md}"
USE_SU="${USE_SU:-0}"
BUILD="${BUILD:-0}"
BUILD_MODE="${BUILD_MODE:-android}"
EXTRA_ENV="${EXTRA_ENV:-}"
CLEAR_LOGCAT="${CLEAR_LOGCAT:-1}"
WAIT_SECS="${WAIT_SECS:-120}"
WAIT_BOOT_COMPLETED="${WAIT_BOOT_COMPLETED:-1}"
START_BROKER="${START_BROKER:-1}"
KILL_STALE_BROKER="${KILL_STALE_BROKER:-1}"
WAIT_BROKER_SECS="${WAIT_BROKER_SECS:-10}"

WORKDIR="${WORKDIR:-/tmp/md-phone-run}"
mkdir -p "${WORKDIR}"

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_PREFIX="${OUT_PREFIX:-${WORKDIR}/phone-${STAMP}}"
OUT_LOG="${OUT_LOG:-${OUT_PREFIX}.out.txt}"
OUT_DMESG="${OUT_DMESG:-${OUT_PREFIX}.dmesg.txt}"
OUT_LOGCAT="${OUT_LOGCAT:-${OUT_PREFIX}.logcat.txt}"

die() { echo "ERROR: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing dependency: $1"; }

need ssh
need scp

# Non-interactive SSH: accept first-time host keys automatically.
# You can override via `SSH_OPTS=...` if you want stricter behavior.
SSH_OPTS="${SSH_OPTS:-"-o StrictHostKeyChecking=accept-new -o BatchMode=yes"}"

if [[ "${BUILD}" == "1" ]]; then
  need make
  echo "== Build test binary (${BUILD_MODE}) =="
  make -C "${TESTS_DIR}" MODE="${BUILD_MODE}" clean all
fi

[[ -f "${LOCAL_BIN}" ]] || die "test binary not found: ${LOCAL_BIN} (set LOCAL_BIN=... or BUILD=1)"
if [[ "${START_BROKER}" == "1" ]]; then
  [[ -f "${LOCAL_BROKER}" ]] || die "broker binary not found: ${LOCAL_BROKER} (set LOCAL_BROKER=... or BUILD=1)"
fi

REMOTE_TMP="md-phone-${STAMP}"
REMOTE_DIR="~/tmp/${REMOTE_TMP}"

echo "== Upload binary to jump box =="
ssh ${SSH_OPTS} "${JUMP_HOST}" "mkdir -p ${REMOTE_DIR}"
scp ${SSH_OPTS} -p "${LOCAL_BIN}" "${JUMP_HOST}:${REMOTE_DIR}/scudo_shared_arena_test"
if [[ "${START_BROKER}" == "1" ]]; then
  scp ${SSH_OPTS} -p "${LOCAL_BROKER}" "${JUMP_HOST}:${REMOTE_DIR}/memory_delegation_broker"
fi

echo "== Run via adb on jump box =="
set +e
ssh ${SSH_OPTS} "${JUMP_HOST}" \
  "REMOTE_TMP=${REMOTE_TMP@Q} PHONE_DIR=${PHONE_DIR@Q} ADB_SERIAL=${ADB_SERIAL@Q} USE_SU=${USE_SU@Q} ADB_BIN=${ADB_BIN@Q} EXTRA_ENV=${EXTRA_ENV@Q} CLEAR_LOGCAT=${CLEAR_LOGCAT@Q} WAIT_SECS=${WAIT_SECS@Q} WAIT_BOOT_COMPLETED=${WAIT_BOOT_COMPLETED@Q} START_BROKER=${START_BROKER@Q} KILL_STALE_BROKER=${KILL_STALE_BROKER@Q} WAIT_BROKER_SECS=${WAIT_BROKER_SECS@Q} bash -s" <<'EOF' 2>&1 | tee "${OUT_LOG}"
set -euo pipefail
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 1; }; }
need sed
need perl

run_timeout() {
  local secs="$1"
  shift
  LC_ALL=C LANG=C perl -e '
    my $secs = shift @ARGV;
    my $pid = fork();
    die "fork failed\n" unless defined $pid;
    if ($pid == 0) {
      exec @ARGV;
      exit 127;
    }
    local $SIG{ALRM} = sub {
      kill "TERM", $pid;
      sleep 1;
      kill "KILL", $pid;
      exit 124;
    };
    alarm $secs;
    waitpid($pid, 0);
    exit(($? >> 8) & 255);
  ' "${secs}" "$@"
}

REMOTE_DIR="${HOME}/tmp/${REMOTE_TMP}"

adb_path=""
if [[ -n "${ADB_BIN}" && -x "${ADB_BIN}" ]]; then
  adb_path="${ADB_BIN}"
else
  adb_path="$(command -v adb || true)"
fi
[[ -n "${adb_path}" ]] || { echo "missing: adb (set ADB_BIN=/path/to/adb)" >&2; exit 1; }

adb_cmd=("${adb_path}")
if [[ -n "${ADB_SERIAL}" ]]; then
  adb_cmd+=( -s "${ADB_SERIAL}" )
fi

echo "adb: devices"
"${adb_cmd[@]}" devices -l </dev/null || true

echo "adb: wait-for-device (timeout=${WAIT_SECS}s)"
ok=0
for ((i=0; i<WAIT_SECS; i++)); do
  st="$("${adb_cmd[@]}" get-state </dev/null 2>/dev/null || true)"
  if [[ "${st}" == "device" ]]; then
    ok=1
    break
  fi
  sleep 1
done
if [[ "${ok}" != "1" ]]; then
  echo "ERROR: adb device not online after ${WAIT_SECS}s" >&2
  "${adb_cmd[@]}" devices -l </dev/null >&2 || true
  exit 1
fi

if [[ "${WAIT_BOOT_COMPLETED}" == "1" ]]; then
  echo "adb: wait boot_completed"
  ok=0
  for ((i=0; i<WAIT_SECS; i++)); do
    boot="$("${adb_cmd[@]}" shell getprop sys.boot_completed </dev/null 2>/dev/null | tr -d '\r' || true)"
    if [[ "${boot}" == "1" ]]; then
      ok=1
      break
    fi
    sleep 1
  done
  if [[ "${ok}" != "1" ]]; then
    echo "ERROR: sys.boot_completed != 1 after ${WAIT_SECS}s" >&2
    exit 1
  fi
fi

if [[ "${CLEAR_LOGCAT}" == "1" ]]; then
  echo "adb: clear logcat buffer"
  "${adb_cmd[@]}" logcat -c </dev/null || true
fi

if [[ "${KILL_STALE_BROKER}" == "1" ]]; then
  echo "adb: kill stale shared arena broker"
  cleanup_cmd='for p in $(pidof memory_delegation_broker 2>/dev/null) $(pidof scudo_shared_arena_test 2>/dev/null); do kill -TERM "$p" || true; done; sleep 1; for p in $(pidof memory_delegation_broker 2>/dev/null) $(pidof scudo_shared_arena_test 2>/dev/null); do kill -KILL "$p" || true; done'
  if [[ "${USE_SU}" == "1" ]]; then
    "${adb_cmd[@]}" shell "su -c $(printf "%q" "${cleanup_cmd}")" </dev/null || true
  else
    "${adb_cmd[@]}" shell "sh -c $(printf "%q" "${cleanup_cmd}")" </dev/null || true
  fi
fi

echo "adb: prepare phone dir ${PHONE_DIR}"
"${adb_cmd[@]}" shell "mkdir -p '${PHONE_DIR}' && chmod 755 '${PHONE_DIR}'" </dev/null

if [[ "${START_BROKER}" == "1" ]]; then
  echo "adb: push broker"
  "${adb_cmd[@]}" push "${REMOTE_DIR}/memory_delegation_broker" "${PHONE_DIR}/memory_delegation_broker" </dev/null
  "${adb_cmd[@]}" shell "chmod 0755 '${PHONE_DIR}/memory_delegation_broker'" </dev/null

  broker_cmd="cd '${PHONE_DIR}' || exit 1; rm -f broker.log broker.pid; ./memory_delegation_broker --trace >broker.log 2>&1 & echo \$! > broker.pid"
  echo "adb: start memory_delegation_broker"
  if [[ "${USE_SU}" == "1" ]]; then
    "${adb_cmd[@]}" shell "su -c $(printf "%q" "${broker_cmd}")" </dev/null
  else
    "${adb_cmd[@]}" shell "sh -c $(printf "%q" "${broker_cmd}")" </dev/null
  fi

  echo "adb: wait broker ready"
  ok=0
  for ((i=0; i<WAIT_BROKER_SECS; i++)); do
    if "${adb_cmd[@]}" shell "grep -q 'ready num_cores=' '${PHONE_DIR}/broker.log'" </dev/null 2>/dev/null; then
      ok=1
      break
    fi
    sleep 1
  done
  if [[ "${ok}" != "1" ]]; then
    echo "ERROR: memory_delegation_broker not ready after ${WAIT_BROKER_SECS}s" >&2
    "${adb_cmd[@]}" shell "cat '${PHONE_DIR}/broker.log' 2>/dev/null || true" </dev/null >&2 || true
    exit 1
  fi
fi

echo "adb: push binary"
"${adb_cmd[@]}" push "${REMOTE_DIR}/scudo_shared_arena_test" "${PHONE_DIR}/scudo_shared_arena_test" </dev/null
"${adb_cmd[@]}" shell "chmod 0755 '${PHONE_DIR}/scudo_shared_arena_test'" </dev/null

run_env="SCUDO_SHARED_ARENA_FORCE=1 SCUDO_SHARED_ARENA_TRACE=1"
if [[ -n "${EXTRA_ENV}" ]]; then
  run_env="${run_env} ${EXTRA_ENV}"
fi
run_cmd="cd '${PHONE_DIR}' && ${run_env} ./scudo_shared_arena_test"

echo "== scudo_shared_arena_test stdout/stderr =="
if [[ "${USE_SU}" == "1" ]]; then
  "${adb_cmd[@]}" shell "su -c $(printf "%q" "${run_cmd}")" </dev/null
else
  "${adb_cmd[@]}" shell "sh -c $(printf "%q" "${run_cmd}")" </dev/null
fi

# echo "== dmesg (tail) =="
# if [[ "${USE_SU}" == "1" ]]; then
#   run_timeout 20 "${adb_cmd[@]}" shell "su -c 'dmesg | tail -n 400'" </dev/null || true
# else
#   run_timeout 20 "${adb_cmd[@]}" shell "dmesg | tail -n 400" </dev/null || true
# fi

# echo "== logcat (dump) =="
# run_timeout 20 "${adb_cmd[@]}" logcat -d -v time </dev/null | tail -n 400 || true
EOF
run_rc=${PIPESTATUS[0]}
set -e

echo "== Save full dmesg/logcat locally =="
ssh ${SSH_OPTS} "${JUMP_HOST}" \
  "ADB_SERIAL=${ADB_SERIAL@Q} USE_SU=${USE_SU@Q} ADB_BIN=${ADB_BIN@Q} bash -s" <<'EOF' > "${OUT_DMESG}"
set -euo pipefail
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 1; }; }
need perl

run_timeout() {
  local secs="$1"
  shift
  LC_ALL=C LANG=C perl -e '
    my $secs = shift @ARGV;
    my $pid = fork();
    die "fork failed\n" unless defined $pid;
    if ($pid == 0) {
      exec @ARGV;
      exit 127;
    }
    local $SIG{ALRM} = sub {
      kill "TERM", $pid;
      sleep 1;
      kill "KILL", $pid;
      exit 124;
    };
    alarm $secs;
    waitpid($pid, 0);
    exit(($? >> 8) & 255);
  ' "${secs}" "$@"
}

adb_path=""
if [[ -n "${ADB_BIN}" && -x "${ADB_BIN}" ]]; then
  adb_path="${ADB_BIN}"
else
  adb_path="$(command -v adb || true)"
fi
[[ -n "${adb_path}" ]] || { echo "missing: adb (set ADB_BIN=/path/to/adb)" >&2; exit 1; }

adb_cmd=("${adb_path}")
if [[ -n "${ADB_SERIAL}" ]]; then
  adb_cmd+=( -s "${ADB_SERIAL}" )
fi

if [[ "${USE_SU}" == "1" ]]; then
  run_timeout 30 "${adb_cmd[@]}" shell "su -c 'dmesg'" </dev/null || true
else
  run_timeout 30 "${adb_cmd[@]}" shell "dmesg" </dev/null || true
fi
EOF

ssh ${SSH_OPTS} "${JUMP_HOST}" \
  "ADB_SERIAL=${ADB_SERIAL@Q} ADB_BIN=${ADB_BIN@Q} bash -s" <<'EOF' > "${OUT_LOGCAT}"
set -euo pipefail
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 1; }; }
need perl

run_timeout() {
  local secs="$1"
  shift
  LC_ALL=C LANG=C perl -e '
    my $secs = shift @ARGV;
    my $pid = fork();
    die "fork failed\n" unless defined $pid;
    if ($pid == 0) {
      exec @ARGV;
      exit 127;
    }
    local $SIG{ALRM} = sub {
      kill "TERM", $pid;
      sleep 1;
      kill "KILL", $pid;
      exit 124;
    };
    alarm $secs;
    waitpid($pid, 0);
    exit(($? >> 8) & 255);
  ' "${secs}" "$@"
}

adb_path=""
if [[ -n "${ADB_BIN}" && -x "${ADB_BIN}" ]]; then
  adb_path="${ADB_BIN}"
else
  adb_path="$(command -v adb || true)"
fi
[[ -n "${adb_path}" ]] || { echo "missing: adb (set ADB_BIN=/path/to/adb)" >&2; exit 1; }

adb_cmd=("${adb_path}")
if [[ -n "${ADB_SERIAL}" ]]; then
  adb_cmd+=( -s "${ADB_SERIAL}" )
fi

run_timeout 30 "${adb_cmd[@]}" logcat -d -v time </dev/null || true
EOF

echo "Outputs:"
echo "  ${OUT_LOG}"
echo "  ${OUT_DMESG}"
echo "  ${OUT_LOGCAT}"

exit "${run_rc}"
