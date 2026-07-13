#make clean
DEBUG_LEVEL=0 ROCKSDB_PLUGINS=zenfs make -j32 db_bench install
cp db_bench ./build
