# WP92 KTX2 fixtures

`bc7_srgb_1x1.ktx2` and `bc5_unorm_1x1.ktx2` are deterministic 1 x 1,
single-level KTX2 files. A 1 x 1 texture has a complete one-level mip chain.
Each file contains one 16-byte BC block and no supercompression.

Regenerate them with:

```powershell
python -B test/fixtures/ktx2/generate_fixtures.py
```

The script writes the KTX2 header, level index, Khronos basic DFD, and already
compressed block bytes directly. This keeps the fixture independent of encoder
version changes. After regeneration, validate the containers with the
KTX-Software tools:

```powershell
ktx validate test/fixtures/ktx2/bc7_srgb_1x1.ktx2
ktx validate test/fixtures/ktx2/bc5_unorm_1x1.ktx2
```

The blocks are test payloads, not third-party image assets. The engine transfers
them unchanged and never performs a CPU decode.
