Benchmark data is not duplicated into this subtree.

Default scripts read SIFT data from the benchmark data directory inside the
repository. You can override the data directory by setting VEXDB_ROOT or by
passing the data path as the third argument:

  VEXDB_ROOT=/path/to/vexdb_lite \
    vexdb_duckdb/test/run_sift_sql_benchmark.sh <build_dir> <dataset> <data_dir>

To generate the fbin data files from an ann-benchmarks HDF5 source, set:

  ANN_HDF5_SRC=/path/to/sift-128-euclidean.hdf5

and run:

  ./build_duck.sh data
