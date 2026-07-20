#!/bin/bash

# check whether an aux_size argument was given
if [ -z "$1" ]; then
    echo "No aux_size provided. Creating ZenFS without aux_size option..."
    ./plugin/zenfs/util/zenfs mkfs --zbd=nvme0n1 --aux_path=/mnt/aux --hyssd --force
else
    AUX_SIZE=$1
    
    # aux_size must be a positive integer
    if ! [[ "$AUX_SIZE" =~ ^[0-9]+$ ]]; then
        echo "Error: aux_size must be a positive number"
        exit 1
    fi
    
    echo "Creating ZenFS with aux_size=${AUX_SIZE}GB..."
    ./plugin/zenfs/util/zenfs mkfs --zbd=nvme0n1 --aux_path=/mnt/aux --hyssd --aux_size=$AUX_SIZE --force
fi

echo "Done!"
