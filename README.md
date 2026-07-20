# HyZNS

![FEMU](https://img.shields.io/badge/femu-v9.0.1-purple)
![Kernel](https://img.shields.io/badge/kernel-v5.15.0-blue)
![F2FS-tools](https://img.shields.io/badge/f2fs--tools-v1.16.0-yellow)
![nvme-cli](https://img.shields.io/badge/nvme--cli-v2.11-yellow)
![rocksdb](https://img.shields.io/badge/rocks--db-v10.1.3-yellow)

```
 _   _ __   __     ______ ______ __  __ _    _ 
| | | |\ \ / /    |  ____|  ____|  \/  | |  | |
| |_| | \   /     | |__  | |__  | \  / | |  | |
|  _  |  | |   +  |  __| |  __| | |\/| | |  | |
| | | |  | |      | |    | |____| |  | | |__| |
|_| |_|  |_|      |_|    |______|_|  |_|\____/  -- A QEMU-based and DRAM-backed NVMe SSD Emulator

```

Contact Information
--------------------

**Maintainer**: [Doeun Kim](https://github.com/ehdms96), Email: ``doeun96@hanyang.ac.kr``

**Developer**: [Junmo Seong](https://github.com/wnsah814), Email: ``wnsah814@hanyang.ac.kr``
               [Jinyoung Kim](https://github.com/jyk122121), Email: ``ffff4001@hanyang.ac.kr``

**Contributor**: [Junill Jang](https://github.com/jang1161), Email: ``jang1161@hanyang.ac.kr``

Project Description (What is FEMU?)
-----------------------------------

                            +--------------------+
                            |    VM / Guest OS   |
                            |                    |
                            |                    |
                            |  NVMe Block Device |
                            +--------^^----------+
                                     ||
                                  PCIe/NVMe
                                     ||
      +------------------------------vv------------------------------+
      |  +---------+ +---------+ +---------+ +---------+ +--------+  |
      |  | Blackbox| |  .....  | | ZNS-SSD | |  .....  | | HY-ZNS |  |
      |  +---------+ +---------+ +---------+ +---------+ +--------+  |
      |                    FEMU NVMe SSD Controller                  |
      +--------------------------------------------------------------+


Briefly speaking, FEMU is a **fast**, **accurate**, **scalable**, and
**extensible** NVMe SSD Emulator. Based upon QEMU/KVM, FEMU is exposed to Guest
OS (Linux) as an NVMe block device (e.g. /dev/nvme0nX). It supports emulating different types of SSDs:

- ``Blackbox mode`` (``BBSSD``) with FTL managed by the device (like most of
  current commercial SSDs). A page-level mapping based FTL is included.

- ``ZNS mode`` (``ZNSSD``), exposing NVMe Zone interface for the host to
  directly read/write/append to the device following certain rules.
  
- ``HY-ZNS mode`` (``HYZNS``), (a.k.a. Hybrid ZNS) Some Zone are managed with host FTL,  
  and the others are exposing NVMe Zone interface for the host to
  directly read/write/append to the device following certain rules.


Run FEMU
--------

### 0. Minimum Requirement

- Run FEMU on a physical machine, not inside a VM (if the VM has nested
  virtualization enabled, you can also give it a try, but FEMU performance will
  suffer, this is **not** recommended.)

- At least 8 cores and 12GB DRAM in the physical machine to enable seamless run
  of the following default FEMU scripts emulating a 4GB SSD in a VM with 4
  vCPUs and 4GB DRAM.

- If you intend to emulate a larger VM (more vCPUs and DRAM) and an SSD with
  larger capacity, make sure refer to the resource provisioning tips
  [here](https://github.com/MoatLab/FEMU/wiki/Before-running-FEMU).

- For more details, please check out the [FEMU wiki](https://github.com/MoatLab/FEMU/wiki).

### 1. Compile FEMU and Boot the Guest VM (host side)

```bash
cd femu
mkdir build-femu && cd build-femu
cp ../femu-scripts/femu-copy-scripts.sh . && ./femu-copy-scripts.sh
sudo ./pkgdep.sh    # one-time: build dependencies
./femu-compile.sh
./run-hyzns.sh      # boots the guest VM with the HyZNS device (8x4 geometry)
```

All remaining steps run **inside the guest VM**.

### 2. Paper Experiments (guest side)

Every figure of the paper is reproduced with a single command. Each
`experiments/<figure>/run.sh` pins the exact parameters used for the published
data, runs the workload, and chains all post-processing, so the plot-ready
CSVs and preview figures land in the printed output path:

See [`experiments/README.md`](experiments/README.md) for the full
figure-to-command map, expected runtimes, and the in-VM build steps.