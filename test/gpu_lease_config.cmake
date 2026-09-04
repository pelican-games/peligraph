# The only timeout policy source for GPU-labelled CTest tests.  Keep these
# values in sync with the command-line defaults consumed by gpu_lease_run.
set(PELICAN_GPU_LEASE_TIMEOUT_MAX 1200)
set(PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT 600)
set(PELICAN_GPU_CTEST_TIMEOUT_GRACE 60)
