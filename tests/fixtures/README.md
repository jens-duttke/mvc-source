# Test fixtures

Small, committed Annex-B H.264 elementary streams used by the regression tests.
They are *inputs* (not golden outputs), so they do not rot across edge264
versions and let the tests run without an encoder installed (the CI `test` job
has no ffmpeg).

## `base_multigop.264`

A 2D stream, 64x64, 12 frames, one IDR every 3 frames (4 closed GOPs), with the
parameter sets repeated at every GOP:

```
SPS PPS SEI IDR* nonIDR* nonIDR*  SPS PPS IDR* nonIDR* nonIDR*  ... (x4 GOPs)
```

(`*` marks the first slice of a coded picture.) Because SPS/PPS are repeated per
GOP, seeking works on this stream as-is; `seektest` derives the harder
topologies from it in memory (no encoder needed) by NAL surgery:

- **headerless-GOP** (SPS/PPS once at the front, no non-VCL NAL between access
  units) - the shape of a default `x264` raw elementary stream. Exercises
  `scan_index`'s access-unit boundary tracking and the parameter-set re-feed on
  seek.
- **AUD-headed, headers once** (an AUD before every access unit, SPS/PPS only at
  the front) - isolates the parameter-set re-feed on seek.

Regenerate (needs ffmpeg with libx264):

```sh
ffmpeg -f lavfi -i "testsrc2=size=64x64:rate=1:duration=12" \
    -c:v libx264 -g 3 -keyint_min 3 -sc_threshold 0 -bf 0 -pix_fmt yuv420p \
    -x264-params "aud=0:repeat-headers=1:annexb=1" \
    -f h264 base_multigop.264
```
