#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_SCRIPT="${SCRIPT_DIR}/run_phone_alloc_trace_pressure_matrix.sh"

RUNS="${RUNS:-3}"
PRESSURE_APPS="${PRESSURE_APPS:-0 1 2 3 4}"
LOCAL_OUT_DIR="${LOCAL_OUT_DIR:-/tmp/md-alloc-trace-pressure-matrix-repeat-$(date +%Y%m%d-%H%M%S)}"

mkdir -p "${LOCAL_OUT_DIR}"

run=1
while [[ "${run}" -le "${RUNS}" ]]; do
  rep_dir="${LOCAL_OUT_DIR}/rep${run}"
  echo "== repeat ${run}/${RUNS}: ${rep_dir} =="
  BUILD="${BUILD:-0}" \
    PRESSURE_APPS="${PRESSURE_APPS}" \
    LOCAL_OUT_DIR="${rep_dir}" \
    "${BASE_SCRIPT}" | tee "${LOCAL_OUT_DIR}/stdout_rep${run}.log"
  cp "${rep_dir}/matrix_results.csv" "${LOCAL_OUT_DIR}/matrix_results_rep${run}.csv"
  run=$((run + 1))
done

{
  echo "mode,apps,runs,avg_events,avg_elapsed_ms,avg_events_per_s,avg_requested_mb,avg_requested_mb_per_s,avg_maxrss_mb,avg_delegated_arena_in_use_mb,avg_delegated_arena_free_reusable_mb,avg_delegated_arena_ready,avg_arena_alloc_attempts,avg_arena_alloc_success,avg_arena_alloc_fallback,avg_group_pss_mb,avg_group_rss_mb,avg_group_peak_pss_mb,avg_delegated_arena_in_use_at_peak_pss_mb,avg_wall_ms,avg_donor_ready,total_alloc_failures,total_rc_failures"
  awk -F, '
    FNR == 1 { next }
    {
      key=$1 "," $2
      count[key]++
      events[key]+=$6
      elapsed[key]+=$7
      eps[key]+=$8
      req[key]+=$9
      reqps[key]+=$10
      maxrss[key]+=$13
      arena_in_use[key]+=$14
      arena_free_reusable[key]+=$15
      arena_ready[key]+=$16
      arena_attempts[key]+=$17
      arena_success[key]+=$18
      arena_fallback[key]+=$19
      pss[key]+=$20
      rss[key]+=$21
      peak_pss[key]+=$22
      arena_in_use_at_peak_pss[key]+=$23
      wall[key]+=$25
      ready[key]+=$4
      failures[key]+=$28
      rc_failures[key]+=($5 != 0)
      seen[key]=1
    }
    END {
      for (key in seen) {
        split(key, a, ",")
        n=count[key]
        printf "%s,%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d\n",
          a[1], a[2], n, events[key]/n, elapsed[key]/n, eps[key]/n,
          req[key]/n, reqps[key]/n, maxrss[key]/n, arena_in_use[key]/n,
          arena_free_reusable[key]/n, arena_ready[key]/n,
          arena_attempts[key]/n, arena_success[key]/n,
          arena_fallback[key]/n,
          pss[key]/n, rss[key]/n, peak_pss[key]/n,
          arena_in_use_at_peak_pss[key]/n, wall[key]/n,
          ready[key]/n, failures[key], rc_failures[key]
      }
    }' "${LOCAL_OUT_DIR}"/matrix_results_rep*.csv | sort -t, -k2,2n -k1,1
} > "${LOCAL_OUT_DIR}/summary_avg.csv"

{
  echo "apps,baseline_events,delegated_events,baseline_requested_mb,delegated_requested_mb,baseline_ms,delegated_ms,speedup,baseline_events_per_s,delegated_events_per_s,baseline_req_mb_s,delegated_req_mb_s,baseline_group_pss_mb,delegated_group_pss_mb,group_pss_delta_mb,baseline_group_peak_pss_mb,delegated_group_peak_pss_mb,group_peak_pss_delta_mb,delegated_arena_in_use_at_peak_pss_mb,arena_alloc_attempts,arena_alloc_success,arena_alloc_fallback,arena_alloc_fallback_rate,baseline_group_rss_mb,delegated_group_rss_mb,group_rss_delta_mb,baseline_maxrss_mb,delegated_maxrss_mb,maxrss_delta_mb,delegated_arena_ready,delegated_arena_in_use_mb,delegated_arena_free_reusable_mb,total_alloc_failures,total_rc_failures"
  awk -F, '
    NR == 1 { next }
    {
      mode=$1
      apps=$2
      events[apps,mode]=$4
      elapsed[apps,mode]=$5
      eps[apps,mode]=$6
      req[apps,mode]=$7
      reqps[apps,mode]=$8
      maxrss[apps,mode]=$9
      arena_in_use[apps,mode]=$10
      arena_free_reusable[apps,mode]=$11
      arena_ready[apps,mode]=$12
      arena_attempts[apps,mode]=$13
      arena_success[apps,mode]=$14
      arena_fallback[apps,mode]=$15
      pss[apps,mode]=$16
      rss[apps,mode]=$17
      peak_pss[apps,mode]=$18
      arena_in_use_at_peak_pss[apps,mode]=$19
      failures[apps]+=$22
      rc_failures[apps]+=$23
      apps_seen[apps]=1
    }
    END {
      for (apps in apps_seen) {
        b=elapsed[apps,"default"]+0
        d=elapsed[apps,"delegated"]+0
        speed=(d > 0) ? b / d : 0
        bp=pss[apps,"default"]+0
        dp=pss[apps,"delegated"]+0
        bpp=peak_pss[apps,"default"]+0
        dpp=peak_pss[apps,"delegated"]+0
        br=rss[apps,"default"]+0
        dr=rss[apps,"delegated"]+0
        bm=maxrss[apps,"default"]+0
        dm=maxrss[apps,"delegated"]+0
        aip=arena_in_use_at_peak_pss[apps,"delegated"]+0
        aa=arena_attempts[apps,"delegated"]+0
        as=arena_success[apps,"delegated"]+0
        afb=arena_fallback[apps,"delegated"]+0
        afr=(aa > 0) ? afb / aa : 0
        ar=arena_ready[apps,"delegated"]+0
        ai=arena_in_use[apps,"delegated"]+0
        af=arena_free_reusable[apps,"delegated"]+0
        printf "%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.6f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d\n",
          apps, events[apps,"default"], events[apps,"delegated"],
          req[apps,"default"], req[apps,"delegated"], b, d, speed,
          eps[apps,"default"], eps[apps,"delegated"],
          reqps[apps,"default"], reqps[apps,"delegated"], bp, dp, dp - bp,
          bpp, dpp, dpp - bpp, aip, aa, as, afb, afr, br, dr, dr - br, bm,
          dm, dm - bm, ar, ai, af, failures[apps],
          rc_failures[apps]
      }
    }' "${LOCAL_OUT_DIR}/summary_avg.csv" | sort -t, -k1,1n
} > "${LOCAL_OUT_DIR}/baseline_vs_delegated_avg.csv"

echo
echo "================ ALLOC TRACE PRESSURE MATRIX SUMMARY ================"
echo "summary=${LOCAL_OUT_DIR}/summary_avg.csv"
echo "---------------------------------------------------------------------"
cat "${LOCAL_OUT_DIR}/summary_avg.csv"
echo "====================================================================="
echo
echo "============ ALLOC TRACE PRESSURE MATRIX BASELINE VS DELEGATED ======"
echo "comparison=${LOCAL_OUT_DIR}/baseline_vs_delegated_avg.csv"
echo "---------------------------------------------------------------------"
cat "${LOCAL_OUT_DIR}/baseline_vs_delegated_avg.csv"
echo "====================================================================="
