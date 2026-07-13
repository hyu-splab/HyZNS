#!/bin/bash
# Huaicheng <huaicheng@cs.uchicago.edu>
# Copy necessary scripts for running FEMU

FSD="../femu-scripts"

CPL=(pkgdep.sh femu-compile.sh _femu_tune.sh measure-setup.sh pin-threads.sh debug-dual-gdb.sh run-whitebox.sh run-blackbox.sh run-nossd.sh run-zns.sh run-hyssd.sh run-hyssd-large.sh run-hyssd-small.sh run-hyhost.sh run-hyhost-dual.sh run-zns-dual.sh run-all-128MB-dev.sh run-Allssd-configR-hyssd.sh pin.sh ftk)

echo ""
echo "==> Copying following FEMU script to current directory:"
for f in "${CPL[@]}"
do
	if [[ ! -e $FSD/$f ]]; then
		echo "Make sure you are under build-femu/ directory!"
		exit
	fi
	cp -r $FSD/$f . && echo "    --> $f"
done
echo "Done!"
echo ""

# Per-machine image paths/ports live in ./femu-local.sh (NOT copied, not in
# git) — the run scripts source it if present. Create it on a fresh machine.
if [[ ! -e ./femu-local.sh ]]; then
	echo "NOTE: no ./femu-local.sh here — run scripts will default to \$HOME/images."
	echo "      Create femu-local.sh with e.g.  IMGDIR=/path/to/images  to override."
fi
