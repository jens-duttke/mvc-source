# mvc-source - AviSynth+ & VapourSynth MVC (3D Blu-ray) source plugins

Source plugins for **H.264/AVC and MVC (3D Blu-ray, stereo)** video, built on
the [edge264-mvc](https://github.com/jens-duttke/edge264-mvc) software decoder.
One shared decode core drives two thin host plugins:

- **VapourSynth** (API4) - `core.mvc.Source(...)`, built as `libvsmvc.so`
- **AviSynth+** - `MVCSource(...)`, built as `libavsmvc.so`

Its reason to exist: there is **no dependency-free MVC (3D Blu-ray) source for
these frameservers on Linux**. FFmpeg drops the MVC dependent view entirely, and
the existing options (DGMVCsourceVS, FRIMSource) are Windows-only and pull in
`libmfxsw64.dll`. `mvc-source` decodes both views in pure, self-contained C:
edge264 is statically linked, so each built plugin is a single shared library
with no external runtime dependency.

Intended workflow: decode a 3D Blu-ray's MVC stream, frame-serve the two views
(e.g. as a top-and-bottom clip), interpolate (e.g. with
[vs-rife](https://github.com/HolyWu/vs-rife)) and re-encode - fully on Linux.

## Status

Working and verified end-to-end, **bit-exact against the edge264 reference on
both hosts** (correct frame count, dimensions, frame-accurate seeking).

- [x] **Decode core** (`src/mvcsource.c`) - open, index, seek, and per-view /
  stacked frame assembly on top of edge264. Host-independent; unit-tested
  without any frameserver runtime (`tests/coretest.c`).
- [x] **VapourSynth glue** (`src/plugin.c`) - the API4 filter `mvc.Source`.
  Covered by `tests/mockhost.c` (drives the built plugin through the real API4)
  and confirmed bit-exact in a real VapourSynth runtime (Core R77).
- [x] **AviSynth+ glue** (`src/avisynth_plugin.cpp`) - the `MVCSource` filter.
  Loaded through a real AviSynth+ runtime by `tests/avshost.c` (its C API) and
  confirmed bit-exact there (AviSynth+ 3.7.3).
- [ ] VUI frame-rate auto-detection; on-disk index cache for fast reopening.
- [ ] Native Windows build (the core currently uses POSIX `mmap`/`open`; a small
  I/O shim would let the same plugins build for Windows hosts).

## Usage

Both plugins expose the same layouts and options; only the call syntax differs
per host.

`stack` selects the layout: `"base"` (left/2D), `"right"`, `"tab"`
(top-and-bottom), `"sbs"` (side-by-side) or `"alt"` (alternating frames: base,
dependent, base, dependent ... - twice the frames at twice the frame rate). On a
2D stream the stacked/alternating modes fall back to the single view. `swaplr`
swaps the two views in any layout (base <-> dependent), so a stream authored
right-eye-first can be flipped without re-authoring.

### VapourSynth

```python
import vapoursynth as vs
core = vs.core
core.std.LoadPlugin("/path/to/libvsmvc.so")

# base + dependent views stacked top-and-bottom (full resolution per eye)
clip = core.mvc.Source(r"movie.264", stack="tab")

# ... interpolate to 60000/1001 with vs-rife, then output/encode ...
```

Signature: `core.mvc.Source(source, stack="base", threads=0, fpsnum=..., fpsden=..., swaplr=0)`.

### AviSynth+

```avisynth
LoadPlugin("/path/to/libavsmvc.so")

# base + dependent views stacked top-and-bottom (full resolution per eye)
MVCSource("movie.264", stack="tab")
```

Signature: `MVCSource(source, stack="base", threads=0, fpsnum=..., fpsden=..., swaplr=false)`.

`fpsnum`/`fpsden` must be given together (edge264's public API does not expose
the VUI rate); the default is 24000/1001.

## Building

Requires a C/C++ compiler and an
[edge264-mvc](https://github.com/jens-duttke/edge264-mvc) source tree (pin a
release tag for reproducible builds). The VapourSynth API4 and AviSynth+ SDK
headers are vendored under `include/`, so no host install is needed to build.

```sh
make EDGE264_SRC=/path/to/edge264-mvc     # builds both plugins + the core tests
./coretest movie.264 2                     # 0=base 1=right 2=tab 3=sbs 4=alt
```

Individual targets: `make libvsmvc.so` (VapourSynth), `make libavsmvc.so`
(AviSynth+). Tested on Linux; CI builds and bit-exact-verifies both plugins
against edge264-mvc `v2026.07.07` and AviSynth+ `v3.7.3`.

## Testing

```sh
make check TEST_FILE=movie.264       # core (all layouts, seek==sequential) +
                                     # VapourSynth glue via the mock API4 host
make check-bitexact TEST_FILE=movie.264   # core frame md5 == an edge264 reference

# end-to-end through a real AviSynth+ (needs libavisynth + ffmpeg); properties,
# all layouts, error paths, then a bit-exact frame md5 vs edge264:
make check-avs TEST_FILE=movie.264
```

`make check` needs no frameserver install. The real-runtime proofs
(`vspipe` for VapourSynth, `check-avs` for AviSynth+) run in CI.

## Layout

- `src/mvcsource.{c,h}` - the host-independent decode core (all the logic;
  shared by both plugins, unit-testable without a frameserver runtime).
- `src/plugin.c` - VapourSynth API4 glue (`mvc.Source`).
- `src/avisynth_plugin.cpp` - AviSynth+ glue (`MVCSource`).
- `tests/coretest.c` - standalone core verification (info, sequential decode,
  seek == sequential, raw-frame dump for cross-checking).
- `tests/mockhost.c` - a mock VapourSynth API4 host driving the built plugin.
- `tests/avshost.c` - loads the built AviSynth+ plugin through a real AviSynth+.
- `include/` - vendored VapourSynth API4 and AviSynth+ SDK headers.

## Related projects

- **[Oku3D](https://oku3d.com)** - *watch everything in 3D.* A real-time 3D
  media player from the same author, built on the same
  [edge264-mvc](https://github.com/jens-duttke/edge264-mvc) decoder for native
  H.264 MVC (3D Blu-ray) playback - and it turns *any* other 2D video or photo
  into stereoscopic 3D on the fly with AI depth estimation, so your whole
  library plays in 3D, not just MVC discs.

## License

BSD-3-Clause (see [LICENSE](LICENSE)). Statically links edge264-mvc
(BSD-3-Clause) and builds against vendored SDK headers: the VapourSynth SDK
(LGPL-2.1-or-later) and the AviSynth+ headers (GPL-2.0-or-later, with the
standard exception permitting independent plugins that use only the documented
interfaces - which is exactly how this plugin uses them).
