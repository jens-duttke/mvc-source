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

## `base_opengop.264`

A 2D stream, 64x64, 20 frames, with **open GOPs**: one IDR at the front and three
further random-access points that are *recovery points* - a non-IDR I picture
behind a `recovery_point` SEI. That is the GOP shape a 3D Blu-ray uses (measured
on a real disc: 103 IDRs against 341 recovery points), and `base_multigop.264`
cannot stand in for it, being closed-GOP throughout.

What it exists to catch: a recovery point's **leading pictures** follow it in
decode order but precede it in display order and reference the previous GOP, so a
cold decode emits them - wrong - before the recovery point itself. A seek point
there is only usable together with the POC-derived display index that separates
the two. Serving a leading picture as the recovery point is the regression v0.4.3
shipped; this fixture reproduces it (verified: reintroducing that bug fails
`seektest`'s `opengop_cold` check at frame 14).

It must be driven by `check_cold_seek`, one fresh source per frame. A reverse read
over one source does not reach the seek path at all: the decoded-frame cache holds
the whole fixture (its floor is 16 MiB, thousands of 64x64 pictures), so the wrong
frame is never requested from the decoder.

Regenerate (needs ffmpeg with libx264; `open-gop=1` is what makes x264 emit the
recovery-point SEI, and `bframes=2` is what gives the recovery points leading
pictures to get wrong):

```sh
ffmpeg -f lavfi -i "testsrc2=size=64x64:rate=1:duration=20" \
    -c:v libx264 -g 5 -keyint_min 5 -sc_threshold 0 -pix_fmt yuv420p \
    -x264-params "open-gop=1:bframes=2:b-adapt=0:aud=0:repeat-headers=1:annexb=1" \
    -f h264 base_opengop.264
```

## `mvc_combined.264` + `mvc_base.264` + `mvc_dependent.mvc`

A small **MVC (stereo)** stream and its **real demuxer split**, for `twofiletest`.
`mvc_combined.264` is the single interleaved MVC elementary stream `mvc_open`
decodes; `mvc_base.264` (a clean 2D-playable AVC stream) and `mvc_dependent.mvc`
(subset SPS + the dependent view, carrying its own parameter sets and SEI) are the
two files `mvc_open2` reads - exactly the pair a 3D-Blu-ray demuxer produces.
`twofiletest` requires `mvc_open2(base, dependent)` to decode bit-exact to
`mvc_open(combined)` across every layout.

The split is a **real tsMuxeR demux**, not synthesised by NAL type. That matters:
a demuxer routes each access unit's dependent-section parameter sets/SEI - which
reuse the base NAL types 7/8/6 - to the dependent file, where they must precede
their slices. A by-type split (an earlier version of this test) misrouted them to
the base file and made `mvc_open2`'s dependent slices decode before their PPS,
which failed the test even though `mvc_open2` is correct (proven bit-exact against
FRIMSource on real BD3D2MK3D demuxes). Using a committed real demux removes that
guesswork. Note `mvc_dependent.mvc` genuinely carries its own PPS (NAL type 8), so
the fixture exercises that dependent-parameter-set path.

Source: the JVT MVC conformance bitstream `MVCDS-4.264` (9 frames), copied here as
`mvc_combined.264`. Regenerate the split with tsMuxeR (any version; track 1 is the
MVC/dependent view, track 2 the AVC/base view):

```sh
# demux.meta:
#   MUXOPT --demux
#   V_MPEG4/ISO/AVC, "MVCDS-4.264", track=2
#   V_MPEG4/ISO/MVC, "MVCDS-4.264", track=1
tsMuxeR demux.meta out_dir/
cp out_dir/MVCDS-4.track_2.264 mvc_base.264
cp out_dir/MVCDS-4.track_1.mvc mvc_dependent.mvc
```
