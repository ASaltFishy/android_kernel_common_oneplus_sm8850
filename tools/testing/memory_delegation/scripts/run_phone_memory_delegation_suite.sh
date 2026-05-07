#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Staged physical-phone smoke suite for memory delegation.
#
# It reuses run_phone_over_ssh_adb.sh and intentionally runs from low-risk
# wiring checks to the full revoke/fault path. Override JUMP_HOST/ADB_SERIAL/
# USE_SU/PHONE_DIR/WAIT_SECS exactly as with the underlying runner.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="${SCRIPT_DIR}/run_phone_over_ssh_adb.sh"

TEST_CPU="${TEST_CPU:-0}"
CROSS_FREE_CPU="${CROSS_FREE_CPU:-1}"
FULL_ROUNDS="${FULL_ROUNDS:-4}"
STRESS_ROUNDS="${STRESS_ROUNDS:-8}"
BUILD="${BUILD:-1}"
BUILD_MODE="${BUILD_MODE:-android}"
USE_SU="${USE_SU:-1}"

run_case() {
  local name="$1"
  local envs="$2"

  echo
  echo "== phone suite: ${name} =="
  BUILD="${BUILD}" BUILD_MODE="${BUILD_MODE}" USE_SU="${USE_SU}" \
  EXTRA_ENV="SCUDO_SHARED_ARENA_TEST_CPU=${TEST_CPU} ${envs}" \
    "${RUNNER}"

  BUILD=0
}

run_case "init-only registration" \
  "SCUDO_SHARED_ARENA_TEST_MODE=init_only"

run_case "single-cpu revoke/free" \
  "SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=${FULL_ROUNDS}"

run_case "cross-cpu free migration" \
  "SCUDO_SHARED_ARENA_TEST_FREE_CPU=${CROSS_FREE_CPU} SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=${FULL_ROUNDS}"

run_case "short stress" \
  "SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=${STRESS_ROUNDS}"

echo
echo "PASS: phone memory delegation suite completed"
