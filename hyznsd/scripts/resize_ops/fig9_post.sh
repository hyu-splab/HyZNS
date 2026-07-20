#!/bin/bash
# fig9_post.sh <run_dir> — (re)build the paper Fig9/Fig10 views from a
# fig9_rww.sh run dir (tmpfs run dir or the collected results/resize_ops copy).
#
#   Fig9 : full timeline + per-event zooms, resize window shaded and
#          phase-subdivided (migration / zone reset / ABA update ...)
#   Fig10: read latency CDFs — PRIMARY (matched/): per event, resize window A
#          vs an equal-duration flat baseline window B (auto-picked stall-free
#          near window, or BASE_AT); the legacy whole-run-excluded CDF is kept
#          for reference only (its tail mixes in compaction stalls from the
#          entire run). All plot data also lands as CSVs (replot-ready).
#
# env: ZOOM_PAD(1x)      per-event zoom padding: seconds ("15") or '<f>x' =
#                         f * event duration, min 0.5s ("1x")
#      WINDOWS("A:B[,A:B]") extra arbitrary zoom ranges (phase-shaded too)
#      BASE_AT("t1,t2")   manual baseline-window starts (rel s), one per event
#                         in order; empty entry = auto (e.g. BASE_AT=",95")
#      MATCH_OP(read)     which op the matched CDF compares (read|write)
#      MATCH_PAD(10)      matched zoom_e<k> csv/png padding, seconds
#      MATCH_ZOOM("A:B[,A:B]") EXPLICIT zoom range per event (rel s): set the
#                         zoom start:end directly; the baseline window is
#                         still drawn in the figure/CSV
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RUN=${1:?usage: fig9_post.sh <run_dir>}; RUN=${RUN%/}
tag=$(basename "$RUN")
cd "$RUN"

# collected dirs carry oplat as a tarball — unpack next to it once
[ -d oplat ] || { [ -f oplat.tar.gz ] && tar xzf oplat.tar.gz; }
[ -f db.log ] || { echo "no db.log in $RUN"; exit 1; }

python3 "$HERE/resize_phases.py" db.log resize_phases.csv kernel.log || true

# RocksDB LOG -> flush/compaction/stall/L0 events for the 3-panel timeline
if [ -f rocksdb_LOG ] && [ ! -s rdb_events.csv ]; then
  python3 "$HERE/parse_rocksdb_events.py" rocksdb_LOG rdb_events.csv \
    | tee rdb_summary.txt || true
fi

# flag sets differ: --rdb is timeline-only, --resize-phases is zoom-only
ARGS=(--events events.csv --phases phases.csv)
TARGS=("${ARGS[@]}")
[ -s rdb_events.csv ] && TARGS+=(--rdb rdb_events.csv)
ZARGS=("${ARGS[@]}")
[ -s resize_phases.csv ] && ZARGS+=(--resize-phases resize_phases.csv)

python3 "$HERE/plot_ops_timeline.py" ops.csv "$tag" "${TARGS[@]}" --title "$tag" \
  | tee timeline_stats.txt || true

python3 "$HERE/plot_ops_zoom.py" ops.csv "$tag" "${ZARGS[@]}" \
  --pad "${ZOOM_PAD:-1x}" --title "$tag" || true
WINDOWS=${WINDOWS:-}
for wdw in ${WINDOWS//,/ }; do
  python3 "$HERE/plot_ops_zoom.py" ops.csv "$tag" "${ZARGS[@]}" \
    --window "$wdw" --title "$tag" || true
done

if [ -d oplat ] && [ -s events.csv ]; then
  # PRIMARY fig10: matched-window CDF (A = resize window vs B = equal-duration
  # flat baseline in the same run)
  MARGS=(--op "${MATCH_OP:-read}" --pad "${MATCH_PAD:-10}" --dump-raw)
  [ -n "${BASE_AT:-}" ] && MARGS+=(--base-at "$BASE_AT")
  [ -n "${MATCH_ZOOM:-}" ] && MARGS+=(--zoom-win "$MATCH_ZOOM")
  python3 "$HERE/resize_matched_cdf.py" . matched "${MARGS[@]}" \
    | tee -a timeline_stats.txt || true
  # bundle: figures + all plot CSVs in one zip
  if [ -d matched ]; then
    rm -f "${tag}_matched.zip"
    zip -qr "${tag}_matched.zip" matched \
      && echo "[fig9_post] matched bundle -> ${tag}_matched.zip"
  fi
  # legacy reference: in-window vs whole-run-excluded
  python3 "$HERE/fig10_cdf.py" oplat events.csv "$tag" | tee -a timeline_stats.txt || true
fi
echo "[fig9_post] done -> $RUN"
