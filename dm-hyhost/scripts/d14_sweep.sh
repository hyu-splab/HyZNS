#!/bin/bash
#
# D14 - R-region size sweep.
#
# For each r_end in the configured list, set up dm-hyhost with that
# R-region size, run a fixed-volume fio random-write workload over
# the R-region, and record throughput / latency / WAF / GC stats.
#
# The workload is constant 4 GiB of 4 KiB random writes restricted to
# offset [0, r_end). Smaller r_end means the workload size ÷ R-region
# size ratio is larger, so more pblocks get overwritten and the GC
# kthread does more migration → higher write amplification.
#
# Output: /tmp/d14_results.csv (columns explained at the top of the file).
#
# Run as root in the guest VM. Assumes:
#   - dm-hyhost.ko built (scripts/build.sh)
#   - /dev/nvme0n1 is the FEMU HYHOSTSSD with timing model
#   - fio + python3 in PATH
set -euo pipefail

cd "$(dirname "$0")/.."

BACKING=/dev/nvme0n1
NAME=hyhost0
WORKLOAD_BYTES=$((4 * 1024 * 1024 * 1024))   # 4 GiB
ZONE_SECTORS=$((1 * 1024 * 1024 * 1024 / 512)) # 1 GiB / 512B
RESULTS=${RESULTS:-/tmp/d14_results.csv}

# r_end values in GiB. Each must be zone-aligned (1 GiB granularity).
R_END_GIB=${R_END_GIB:-"2 4 8 16 32 64"}

if ! lsmod | awk '{print $1}' | grep -qx dm_hyhost; then
    ./scripts/load.sh >/dev/null
fi

extract() {
    # extract <key> from a "key=value key=value ..." status line
    local line=$1
    local key=$2
    echo "$line" | grep -oE "${key}=[0-9]+" | head -1 | cut -d= -f2
}

reset_device_r_end() {
    # Move FEMU device r_end back to nsze (entire R) so dm setup can
    # later re-shrink it to whatever the iteration wants. Idempotent.
    nvme io-passthru "$BACKING" --opcode=0x79 --namespace-id=1 \
        --cdw10=268435456 --cdw11=0 --cdw13=0x20 -r >/dev/null 2>&1 || true
}

echo "r_end_gib,r_end_lba,bw_mib_s,iops,p99_us,user_writes,inplace,gc_runs,gc_mig,gc_skip,recycles,nospc,requeue,waf" \
    > "$RESULTS"

for r_gib in $R_END_GIB; do
    r_end=$((r_gib * ZONE_SECTORS))

    echo "=== r_end=${r_gib} GiB (sectors=${r_end}) ==="

    dmsetup remove "$NAME" 2>/dev/null || true
    reset_device_r_end
    ./scripts/setup.sh "$BACKING" "$r_end" "$NAME" >/dev/null

    if [ -n "${GC_POLICY:-}" ]; then
        dmsetup message "$NAME" 0 gc_policy "$GC_POLICY" >/dev/null
    fi

    # snapshot before workload
    before=$(dmsetup status "$NAME" | sed 's/^.*hyhost //')

    # 4 GiB random write across the R-region
    # --runtime caps each iteration so a heavily-GC'd small r_end can't
    # stall the sweep indefinitely; throughput collapse will still show
    # up clearly in the resulting bw/iops/p99 numbers. fio prints a
    # "fio: ... err=N" prefix on stdout before the JSON when a job hits
    # an error, so strip everything before the first '{' before parsing.
    fio_raw=$(fio --name=d14 --filename="/dev/mapper/$NAME" --rw=randwrite \
        --bs=4k --io_size="$WORKLOAD_BYTES" --offset=0 --size=$((r_end * 512)) \
        --direct=1 --ioengine=psync --numjobs=1 \
        --random_generator=tausworthe --norandommap --randrepeat=0 \
        --runtime=300 \
        --output-format=json 2>/dev/null) || true
    fio_json=$(echo "$fio_raw" | sed -n '/^{/,$p')

    # Single-pass extraction; tolerates errored runs by defaulting missing
    # fields to 0 instead of crashing the whole sweep.
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
    bw_mib_s=$(python3 -c "print(round($bw_kib_s/1024, 2))")

    after=$(dmsetup status "$NAME" | sed 's/^.*hyhost //')

    user_writes=$(extract "$after" w)
    inplace=$(extract "$after" inplace)
    gc_runs=$(extract "$after" gc_runs)
    gc_mig=$(extract "$after" gc_mig)
    gc_skip=$(extract "$after" gc_skip)
    recycles=$(extract "$after" recycles)
    nospc=$(extract "$after" nospc)
    requeue=$(extract "$after" requeue)

    # Subtract the warm-up "before" deltas (udev/probe noise) from user
    # writes so WAF reflects the workload only. inplace/gc/recycle are
    # almost zero pre-workload.
    bw_user=$(extract "$before" w)
    bi_inplace=$(extract "$before" inplace)
    bg_mig=$(extract "$before" gc_mig)
    user_writes_d=$((user_writes - bw_user))
    inplace_d=$((inplace - bi_inplace))
    gc_mig_d=$((gc_mig - bg_mig))

    total_user=$((user_writes_d + inplace_d))
    if [ "$total_user" -gt 0 ]; then
        waf=$(python3 -c "print(round(($total_user + $gc_mig_d)/$total_user, 3))")
    else
        waf="nan"
    fi

    printf "  bw=%s MiB/s iops=%s p99=%sus uw=%s inplace=%s gc_runs=%s gc_mig=%s waf=%s\n" \
        "$bw_mib_s" "$iops" "$p99" "$user_writes_d" "$inplace_d" "$gc_runs" "$gc_mig_d" "$waf"

    echo "$r_gib,$r_end,$bw_mib_s,$iops,$p99,$user_writes_d,$inplace_d,$gc_runs,$gc_mig_d,$gc_skip,$recycles,$nospc,$requeue,$waf" \
        >> "$RESULTS"
done

dmsetup remove "$NAME" 2>/dev/null || true
reset_device_r_end

echo
echo "==="
echo "Results in $RESULTS"
column -t -s, "$RESULTS"
