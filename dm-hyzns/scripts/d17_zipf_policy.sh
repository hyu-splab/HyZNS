#!/bin/bash
#
# D17z - GC policy (Greedy vs Cost-Benefit) under skewed vs uniform load.
#
# The d14-shape uniform sweep cannot separate the policies: every line
# ages at the same rate, so both pick the same victims. This harness
# creates the steady state where the policies CAN differ:
#
#   1. Pre-fill a fixed working set (PREFILL_MIB) sequentially, so
#      utilization is high from the start and every subsequent write is
#      an overwrite (displacement churn, no footprint growth).
#   2. Overwrite for RUNTIME seconds with either a zipf:THETA or a
#      uniform random distribution, 4 KiB psync depth-1.
#   3. Record per-cell deltas (measured phase only) + fio latencies.
#
# With zipf, hot pages concentrate invalidation in recently-written
# lines while cold pages plateau old lines at high valid counts -
# Cost-Benefit's age term should beat Greedy's min-valid there; under
# uniform they should tie.
#
# Output: CSV at $RESULTS (default /tmp/d17z_results.csv).
set -euo pipefail

cd "$(dirname "$0")/.."

BACKING=/dev/nvme0n1
NAME=hyzns0
ZONE_SECTORS=$((1024 * 1024 * 1024 / 512))      # 1 GiB zone, 512B sectors
R_GIB=${R_GIB:-8}
PREFILL_MIB=${PREFILL_MIB:-6656}                 # 6.5 GiB on 8 lines = 81% util
RUNTIME=${RUNTIME:-300}
THETA=${THETA:-1.2}
IOENGINE=${IOENGINE:-psync}
IODEPTH=${IODEPTH:-1}
HOT_SECS=${HOT_SECS:-20}
COLD_SECS=${COLD_SECS:-10}
RESULTS=${RESULTS:-/tmp/d17z_results.csv}
POLICIES=${POLICIES:-"greedy cb"}
DISTS=${DISTS:-"zipf uniform"}

R_END=$((R_GIB * ZONE_SECTORS))

if ! lsmod | awk '{print $1}' | grep -qx dm_hyzns; then
    ./scripts/load.sh >/dev/null
fi

extract() {
    local line=$1 key=$2
    echo "$line" | grep -oE "${key}=[0-9]+" | head -1 | cut -d= -f2
}

echo "policy,dist,bw_mib_s,iops,p99_us,user_writes,inplace,gc_runs,gc_mig,gc_skip,recycles,nospc,requeue,waf" \
    > "$RESULTS"

for policy in $POLICIES; do
    for dist in $DISTS; do
        echo "=== cell policy=${policy} dist=${dist} ==="

        dmsetup remove "$NAME" 2>/dev/null || true
        # device boundary to full R (SLBA wire), dm boundary to R_GIB.
        # The grow is refused unless every reshaped S-zone is EMPTY, so
        # verify via BLKREPORTABA and fail loudly instead of letting a
        # silently-stale device boundary EIO the run mid-prefill.
        nvme io-passthru "$BACKING" --opcode=0x79 --namespace-id=1 \
            --cdw10=268435456 --cdw11=0 --cdw13=0x20 -r >/dev/null 2>&1 || true
        nr_rz=$(python3 -c "
import fcntl, struct, os
fd = os.open('$BACKING', os.O_RDONLY)
buf = bytearray(4)
fcntl.ioctl(fd, (2<<30)|(4<<16)|(0x12<<8)|138, buf)
print(struct.unpack('<I', bytes(buf))[0])
os.close(fd)")
        if [ "$nr_rz" -ne 128 ]; then
            echo "ERROR: device r_end reset failed (nr_rzones=$nr_rz, want 128)." >&2
            echo "       Non-EMPTY S zones block the grow: blkzone reset them first." >&2
            exit 1
        fi
        ./scripts/setup.sh "$BACKING" "$R_END" "$NAME" >/dev/null
        dmsetup message "$NAME" 0 gc_policy "$policy" >/dev/null

        # steady-state precondition: sequential fill of the working set
        if ! fio --name=prefill --filename="/dev/mapper/$NAME" --rw=write \
            --bs=1m --io_size="${PREFILL_MIB}m" --offset=0 \
            --size="${PREFILL_MIB}m" --direct=1 --ioengine=psync \
            --output-format=json > /tmp/d17z_prefill.json 2>&1; then
            echo "ERROR: prefill failed (see /tmp/d17z_prefill.json, dmesg)" >&2
            exit 1
        fi

        before=$(dmsetup status "$NAME" | sed 's/^.*hyzns //')

        if [ "$dist" = "phased" ]; then
            # Temporal hot/cold separation: alternate bursts confined to
            # the first 10% of the range (hot) and the remaining 90%
            # (cold). Lines written during a phase are temperature-pure,
            # which is what gives Cost-Benefit's age term something to
            # exploit; stationary mixes make every line identical and
            # the policies tie structurally.
            hot_mib=$((PREFILL_MIB / 10))
            cold_mib=$((PREFILL_MIB - hot_mib))
            phase_end=$(( $(date +%s) + RUNTIME ))
            while [ "$(date +%s)" -lt "$phase_end" ]; do
                fio --name=hot --filename="/dev/mapper/$NAME" \
                    --rw=randwrite --bs=4k --offset=0 --size="${hot_mib}m" \
                    --direct=1 --ioengine="$IOENGINE" --iodepth="$IODEPTH" \
                    --numjobs=1 --random_generator=tausworthe \
                    --norandommap --randrepeat=0 --time_based \
                    --runtime="$HOT_SECS" --output-format=json \
                    >/dev/null 2>&1 || { echo "ERROR: hot phase fio failed" >&2; exit 1; }
                fio --name=cold --filename="/dev/mapper/$NAME" \
                    --rw=randwrite --bs=4k --offset="${hot_mib}m" \
                    --size="${cold_mib}m" \
                    --direct=1 --ioengine="$IOENGINE" --iodepth="$IODEPTH" \
                    --numjobs=1 --random_generator=tausworthe \
                    --norandommap --randrepeat=0 --time_based \
                    --runtime="$COLD_SECS" --output-format=json \
                    >/dev/null 2>&1 || { echo "ERROR: cold phase fio failed" >&2; exit 1; }
            done
            bw_kib_s=0; iops=0; p99=0
        else
            DIST_ARGS=""
            if [ "$dist" = "zipf" ]; then
                DIST_ARGS="--random_distribution=zipf:${THETA}"
            fi
            fio_raw=$(fio --name=d17z --filename="/dev/mapper/$NAME" \
                --rw=randwrite --bs=4k --offset=0 --size="${PREFILL_MIB}m" \
                --direct=1 --ioengine="$IOENGINE" --iodepth="$IODEPTH" \
                --numjobs=1 \
                --random_generator=tausworthe --norandommap --randrepeat=0 \
                $DIST_ARGS --time_based --runtime="$RUNTIME" \
                --output-format=json 2>/dev/null) || true
            fio_json=$(echo "$fio_raw" | sed -n '/^{/,$p')

            read bw_kib_s iops p99 < <(echo "$fio_json" | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
    w = d['jobs'][0]['write']
    bw = w.get('bw', 0)
    iops = int(w.get('iops', 0))
    pct = w.get('clat_ns', {}).get('percentile', {})
    p99 = int(pct.get('99.000000', 0) / 1000)
except Exception:
    bw, iops, p99 = 0, 0, 0
print(bw, iops, p99)
")
        fi
        bw_mib_s=$(python3 -c "print(round($bw_kib_s/1024, 2))")

        after=$(dmsetup status "$NAME" | sed 's/^.*hyzns //')

        d() { echo $(( $(extract "$after" "$1") - $(extract "$before" "$1") )); }
        uw=$(d w); ip=$(d inplace); gr=$(d gc_runs); gm=$(d gc_mig)
        gs=$(d gc_skip); rc=$(d recycles); ns=$(d nospc); rq=$(d requeue)

        if [ "$dist" = "phased" ]; then
            bw_mib_s=$(python3 -c "print(round($uw*4/1024/$RUNTIME, 2))")
            iops=$((uw / RUNTIME))
        fi

        total_user=$((uw + ip))
        if [ "$total_user" -gt 0 ]; then
            waf=$(python3 -c "print(round(($total_user + $gm)/$total_user, 3))")
        else
            waf="nan"
        fi

        printf "  bw=%s MiB/s iops=%s p99=%sus uw=%s inplace=%s gc_runs=%s gc_mig=%s waf=%s\n" \
            "$bw_mib_s" "$iops" "$p99" "$uw" "$ip" "$gr" "$gm" "$waf"
        echo "${policy},${dist},${bw_mib_s},${iops},${p99},${uw},${ip},${gr},${gm},${gs},${rc},${ns},${rq},${waf}" \
            >> "$RESULTS"
    done
done

echo "results: $RESULTS"
