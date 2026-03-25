#!/usr/bin/env bash
set -euo pipefail

REMOTE=${REMOTE:-yogendra@lpdquad.epfl.ch}
MINMAX_ROOT=${MINMAX_ROOT:-/localhome/yogendra/CBAT-main-minmax}
BASE_ROOT=${BASE_ROOT:-/localhome/yogendra/CBAT-main}
# Use multiple worker threads for the measured phase; keep prefill single-threaded
# because the concurrent prefill path is unstable in this environment.
COMMON_ARGS='-nwork 4 -nprefill 1 -prefill-insert -prefillsize 50000 -k 100000 -t 3000'

run_case() {
  local repo_root="$1"
  local label="$2"
  local mode="$3"
  echo "=== ${label} ${mode} ==="
  ssh "$REMOTE" "set -e; cd ${repo_root}/microbench && /usr/bin/time -p env LD_PRELOAD=../lib/libjemalloc.so numactl --interleave=all ./bin/ubench_brown_ext_chromatic_augment_lf.alloc_new.reclaim_debra.pool_none.out ${COMMON_ARGS} ${mode} >/dev/null"
}

run_case "$MINMAX_ROOT" "CBAT-main-minmax" "-min"
run_case "$BASE_ROOT" "CBAT-main" "-min"
run_case "$MINMAX_ROOT" "CBAT-main-minmax" "-max"
run_case "$BASE_ROOT" "CBAT-main" "-max"
