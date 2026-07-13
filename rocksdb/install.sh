#!/bin/bash

DEBUG_LEVEL=0 ROCKSDB_PLUGINS=zenfs make -j48 db_bench install
cd plugin/zenfs/util
make clean;
make
cd ../../../
cp db_bench build

