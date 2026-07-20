#!/bin/bash
# selftest.sh — offline validation of hyznsd policy with mock device/report
# inputs (no live FEMU/dm/FS needed). Exercises the line/zone unit handling
# (default line=128 MiB, zone=1 GiB=8 lines; run with LPB=16384 for the
# 64 MiB-line layout) AND the grow/shrink/steady/guard/clamp decisions,
# in --dry-run --once, checking the verdict.
#
# The mock emits the current dm-hyzns status (line_pblocks, zone_pblocks,
# free_pages, ...). The single most important assertion is the cur_R
# round-trip: set R=<z> zones and the daemon must report curR=<z>, NOT <z>*16
# (a line/zone mix-up would feed an inflated zone number to resize_cns).
set -u
cd "$(dirname "$0")/.."
BIN=./hyznsd
[ -x "$BIN" ] || { echo "build first: make"; exit 1; }

LPB=${LPB:-32768}           # line_pblocks: 128 MiB line (8x8; LPB=16384 tests the 64 MiB-line layout)
ZPB=262144                  # zone_pblocks: 1 GiB zone (= ZPB/LPB lines)
ZS=2097152                  # zone sectors (zone_pblocks*8) = 1 GiB
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0

# emit a dmsetup status line:  mk_dm <cur_r_zones> <free_gib> [nospc] [valid_gib]
#   free capacity is given in ZONES/GiB; free_pages = free_gib*ZPB (what FTR
#   reads), free_lines = free_gib*16 (allocator lines, display).
mk_dm() {
  local cur_r=$1 free_gib=$2 nospc=${3:-0} valid_gib=${4:-0}
  local r_end=$(( cur_r * ZS ))
  local free_pages=$(( free_gib * ZPB ))
  local free_lines=$(( free_gib * (ZPB / LPB) ))
  local valid_pages=$(( valid_gib * ZPB ))
  printf '0 536870912 hyzns r_end=%d pages=0 lines=0 line_pblocks=%d zone_pblocks=%d block_pages=512 gc_policy=greedy free_lines=%d dirty=0 cur_off=0 r_bios=0 s_bios=0 splits=0 w=0 r=0 unmapped=0 ovr=0 inplace=0 recycles=0 gc_runs=0 gc_mig=0 gc_skip=0 erases=0 erase_fail=0 requeue=0 nospc=%d wfail=0 discards=0 discard_pgs=0 valid_pages=%d valid_lines=0 vh=0:0:0:0:0 best_victim_vpc=0 free_pages=%d gc_blocked=0\n' \
    "$r_end" "$LPB" "$ZPB" "$free_lines" "$nospc" "$valid_pages" "$free_pages"
}

# emit a report-zones text: mk_rz <cur_r> <total> <free_s_zones>
mk_rz() {
  local cur_r=$1 total=$2 freez=$3 i state slba
  for ((i=0; i<total; i++)); do
    slba=$(( i * ZS ))
    if (( i < cur_r )); then state=FULL
    elif (( i < cur_r + freez )); then state=EMPTY
    else state=FULL; fi
    printf 'SLBA: 0x%x  WP: 0x0  Cap: 0x%x  State: %s  Type: SEQWRITE_REQ  Attrs: 0x0\n' \
      "$slba" "$ZS" "$state"
  done
}

# run one case: check <name> <expect GROW|SHRINK|NONE> [target] [curR] -- <args...>
check() {
  local name=$1 expect=$2; shift 2
  local exp_tgt="" exp_cur=""
  if   [[ "${1:-}" =~ ^[0-9]+$ ]]; then exp_tgt=$1; shift
  elif [ "${1:-}" = "-" ];        then shift; fi          # "-" = don't assert target
  if [[ "${1:-}" =~ ^[0-9]+$ ]]; then exp_cur=$1; shift; fi
  [ "${1:-}" = "--" ] && shift
  local out; out=$("$BIN" --once --dry-run -v "$@" 2>&1)
  local line; line=$(echo "$out" | grep -E ' -> (GROW|SHRINK|NONE) ')
  local got_act got_tgt got_cur
  got_act=$(echo "$line" | grep -oE '\-> (GROW|SHRINK|NONE)' | awk '{print $2}')
  got_tgt=$(echo "$line" | grep -oE 'target=[0-9]+' | cut -d= -f2)
  got_cur=$(echo "$line" | grep -oE 'curR=[0-9]+' | cut -d= -f2)
  local ok=1 why=""
  [ "$got_act" = "$expect" ] || { ok=0; why="act"; }
  [ -n "$exp_tgt" ] && [ "$got_tgt" != "$exp_tgt" ] && { ok=0; why="$why target"; }
  [ -n "$exp_cur" ] && [ "$got_cur" != "$exp_cur" ] && { ok=0; why="$why curR"; }
  if [ "$ok" = 1 ]; then
    printf '  PASS  %-30s %s%s%s\n' "$name" "$got_act" \
      "${exp_tgt:+ target=$got_tgt}" "${exp_cur:+ curR=$got_cur}"
    pass=$((pass+1))
  else
    printf '  FAIL  %-30s [%s] expected %s%s%s got %s target=%s curR=%s\n' \
      "$name" "$why" "$expect" "${exp_tgt:+/$exp_tgt}" "${exp_cur:+ curR=$exp_cur}" \
      "$got_act" "${got_tgt:-?}" "${got_cur:-?}"
    echo "$out" | sed 's/^/        | /'
    fail=$((fail+1))
  fi
}

echo "hyznsd offline policy self-test (line/zone unit handling + policy decisions)"
COMMON=(--set rz_source=report)

echo "-- cur_R round-trip (line/zone unit regression) --"
# R=8 zones: daemon MUST report curR=8, not 128 (line-count vs zone-count mix-up).
mk_dm 8 5 >"$T/rt"; mk_rz 8 256 100 >"$T/rtz"
check "curR round-trip R=8"    NONE - 8  -- "${COMMON[@]}" --set mock_dm="$T/rt" --set mock_rz="$T/rtz"
mk_dm 40 10 >"$T/rt2"; mk_rz 40 256 100 >"$T/rtz2"
check "curR round-trip R=40"   NONE - 40 -- "${COMMON[@]}" --set mock_dm="$T/rt2" --set mock_rz="$T/rtz2"

echo "-- FTR (default, paper 2/5/3/4): grow R_C<2*S_Z & R_Z>=5 ; shrink R_C>3*S_Z & R_Z<4 --"
# grow: 1 GiB free (<2 GiB), R_Z=200>3 -> grow 8->9
mk_dm 8 1 >"$T/g"; mk_rz 8 256 200 >"$T/gz"
check "FTR grow (<2 GiB free)"  GROW 9 8 -- "${COMMON[@]}" --set mock_dm="$T/g" --set mock_rz="$T/gz"
# steady: 2 GiB free (not <2), plenty S -> NONE
mk_dm 8 2 >"$T/s"; mk_rz 8 256 200 >"$T/sz"
check "FTR steady (2 GiB free)"  NONE 8 8 -- "${COMMON[@]}" --set mock_dm="$T/s" --set mock_rz="$T/sz"
# shrink: 4 GiB free (>3), R_Z=1<4 -> shrink 8->7
mk_dm 8 4 >"$T/h"; mk_rz 8 256 1 >"$T/hz"
check "FTR shrink (>3 GiB, S low)" SHRINK 7 8 -- "${COMMON[@]}" --set mock_dm="$T/h" --set mock_rz="$T/hz"
# grow blocked: <2 GiB free but R_Z=3 (<5) -> NONE
mk_dm 8 1 >"$T/gb"; mk_rz 8 256 3 >"$T/gbz"
check "FTR grow blocked (R_Z=3)" NONE 8 8 -- "${COMMON[@]}" --set mock_dm="$T/gb" --set mock_rz="$T/gbz"
# shrink blocked: >3 GiB free but R_Z=4 (not <4) -> NONE
mk_dm 8 4 >"$T/hb"; mk_rz 8 256 4 >"$T/hbz"
check "FTR shrink blocked (R_Z=4)" NONE 8 8 -- "${COMMON[@]}" --set mock_dm="$T/hb" --set mock_rz="$T/hbz"
# configurable thresholds: same states flip with --set overrides
mk_dm 8 4 >"$T/ho"; mk_rz 8 256 2 >"$T/hoz"   # R_Z=2: shrinks by default (2<4)...
check "FTR shrink_rz=2 blocks R_Z=2" NONE 8 8 -- "${COMMON[@]}" --set ftr_shrink_rz=2 --set mock_dm="$T/ho" --set mock_rz="$T/hoz"
mk_dm 8 1 >"$T/go"; mk_rz 8 256 3 >"$T/goz"   # R_Z=3: blocked by default (<5)...
check "FTR grow_rz=3 allows R_Z=3" GROW 9 8 -- "${COMMON[@]}" --set ftr_grow_rz=3 --set mock_dm="$T/go" --set mock_rz="$T/goz"

echo "-- FTR clamps: r_max blocks grow, r_min blocks shrink --"
# grow blocked by r_max: cur_r=64=r_max, free<2, R_Z plentiful -> NONE
mk_dm 64 1 >"$T/l4"; mk_rz 64 256 100 >"$T/l4z"
check "FTR grow blocked (r_max)" NONE 64 64 -- "${COMMON[@]}" --set mock_dm="$T/l4" --set mock_rz="$T/l4z"
# shrink blocked by r_min: cur_r=4=r_min, free 6 GiB(>3), R_Z=0<4 -> NONE
mk_dm 4 6 >"$T/l6"; mk_rz 4 256 0 >"$T/l6z"
check "FTR shrink blocked (r_min)" NONE 4 4 -- "${COMMON[@]}" --set mock_dm="$T/l6" --set mock_rz="$T/l6z"

echo "-- backward compat: legacy status (no zone_pblocks, line==zone) --"
# old mock: line_pblocks=262144, no zone_pblocks; daemon falls back so R=6 reads curR=6
oldZS=2097152
printf '0 536870912 hyzns r_end=%d pages=0 lines=0 line_pblocks=262144 block_pages=512 gc_policy=greedy free_lines=1 dirty=0 cur_off=0 nospc=0 valid_pages=0 valid_lines=0 free_pages=262144 gc_blocked=0\n' \
  $(( 6 * oldZS )) >"$T/old"; mk_rz 6 256 200 >"$T/oldz"
check "legacy fallback (R=6)"  GROW 7 6 -- "${COMMON[@]}" --set mock_dm="$T/old" --set mock_rz="$T/oldz"

echo "----"
echo "self-test: $pass passed, $fail failed"
[ "$fail" = 0 ]
