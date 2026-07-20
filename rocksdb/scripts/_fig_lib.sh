#!/bin/bash
#
# _fig_lib.sh — shared helpers for the Fig7/Fig8 device-validation harnesses.
# Source it (don't run):   source "$SCRIPTDIR/_fig_lib.sh"
#
# The functions read these globals set by each caller's config block:
#   DBBENCH FS_URI NUM KEY VAL COMP JOBS STATS_SEC RUNDIR MODE GEOM DMNAME
#   CSV TS DRYRUN

# aba_ok <dev> -> 0 if the device answers the vendor ABA report
# (BLKREPORTABA = _IOR(0x12,138,__u32)). Only a HyZNS namespace
# (femu_mode=5) implements it; a pure ZNS device (femu_mode=3) does not, so
# this is what tells the two apart — both report queue/zoned=host-managed.
aba_ok() {
    python3 - "$1" <<'PY' 2>/dev/null
import fcntl, os, sys
try:
    fd = os.open(sys.argv[1], os.O_RDONLY)
    fcntl.ioctl(fd, (2 << 30) | (4 << 16) | (0x12 << 8) | 138, bytearray(4))
    os.close(fd); sys.exit(0)
except Exception:
    sys.exit(1)
PY
}

# list_zoned_nvme -> print every host-managed zoned nvme (one per line)
list_zoned_nvme() {
    local d
    for d in /sys/block/nvme*; do
        [[ -r $d/queue/zoned ]] || continue
        grep -q host-managed "$d/queue/zoned" && echo "/dev/$(basename "$d")"
    done
}

# parse_log <log> <threads> — append every db_bench summary line in <log> to
# the cumulative CSV, one row per benchmark. rww-writes (the writer side the
# patched db_bench prints) is always thread-count 1.
parse_log() {
    local log=$1 threads=$2
    awk -v OFS=, -v ts="$(date -u +%FT%TZ)" -v tag="$TS" -v mode="$MODE" -v geom="$GEOM" \
        -v thr="$threads" -v num="$NUM" -v logf="$log" '
        BEGIN {
            map["fillseq"]="FS";    map["readseq"]="RS"
            map["fillrandom"]="FR"; map["overwrite"]="OW"
            map["readrandom"]="RR"
            map["readwhilewriting"]="RWW-R"; map["rww-writes"]="RWW-W"
        }
        ($1 in map) && $2==":" {
            mb=""; if ($8=="MB/s") mb=$7
            t=thr; if ($1=="rww-writes") t=1
            print ts, tag, mode, geom, map[$1], t, num, $3, $5, mb, logf
            if ($1=="readwhilewriting") { r_ops=$5; r_mb=mb; have_r=1 }
            if ($1=="rww-writes")       { w_ops=$5; w_mb=mb; have_w=1 }
        }
        END {
            # RWW-T = combined read+write throughput during the RWW window
            # (reader + writer run concurrently, so summing their rates gives
            #  the total ops/s the device sustained). ops summed; MB/s summed
            #  where present (the readwhilewriting line prints no read MB/s, so
            #  RWW-T MB/s is ~the write side). micros/op is left blank (a rate
            #  sum has no single per-op latency).
            if (have_r && have_w) {
                tmb = (r_mb=="" ? 0 : r_mb) + (w_mb=="" ? 0 : w_mb)
                print ts, tag, mode, geom, "RWW-T", 2, num, "", r_ops+w_ops, tmb, logf
            }
        }' "$log" >> "$CSV"
}

# run_bench <inv> <benchmarks> <threads> <reads|-1> <existing 0|1> [record 0|1]
# Runs one db_bench invocation against the current FS_URI, echoes its summary
# lines, snapshots dmstatus (hyzns), and appends to the CSV. Returns non-zero
# (without aborting the caller) if db_bench fails.
#   reads is db_bench's --reads = reads PER THREAD (total = reads * threads).
#   record=0 runs the invocation but keeps it OUT of the CSV — used for the
#   fillseq prep that populates a DB before a read workload (RS/RR/RWW), so the
#   prep's fillseq line is not logged as an FS result.
run_bench() {
    local inv=$1 benches=$2 threads=$3 reads=$4 existing=$5 record=${6:-1}
    local log="$RUNDIR/${inv}.log" extra=()
    (( reads >= 0 )) && extra+=(--reads="$reads")
    (( existing ))   && extra+=(--use_existing_db=1)
    local cmd=("$DBBENCH" --fs_uri="$FS_URI" --benchmarks="${benches},stats"
        --use_direct_io_for_flush_and_compaction
        --num="$NUM" --key_size="$KEY" --value_size="$VAL"
        --compression_type="$COMP" --threads="$threads" --max_background_jobs="$JOBS"
        --histogram --statistics --stats_interval_seconds="$STATS_SEC" "${extra[@]}")
    echo "== [$MODE/$inv] $benches  threads=$threads reads=$reads existing=$existing record=$record -> $log"
    if (( DRYRUN )); then printf '   %q' "${cmd[@]}"; echo; return 0; fi
    "${cmd[@]}" &> "$log" \
        || { echo "  !! $inv FAILED:"; tail -5 "$log" | sed 's/^/  | /'; return 1; }
    [[ $MODE == hyzns ]] && dmsetup status "$DMNAME" > "$RUNDIR/dmstatus_${inv}.txt" 2>/dev/null
    grep -E '^(fillseq|readseq|fillrandom|overwrite|readrandom|readwhilewriting|rww-writes) +:' "$log" | sed 's/^/   /'
    (( record )) && parse_log "$log" "$threads"
    return 0
}
