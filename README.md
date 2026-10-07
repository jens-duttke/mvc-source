# mvc-source

mvc-source is a source plugin for **VapourSynth** and **AviSynth+** that decodes **H.264 MVC**, the stereoscopic 3D format of **3D Blu-ray** discs, and serves both eye views as a clip - one view, both stacked top-and-bottom or side by side, or alternating frame by frame. FFmpeg-based sources decode only the base view of these streams and drop the second eye, and the existing MVC sources (DGMVCsourceVS, FRIMSource) run only on Windows and need Intel's `libmfxsw64.dll`. mvc-source runs on Linux and Windows and has no runtime dependency: each plugin is a single `.so` / `.dll`. It also decodes ordinary 2D H.264.

It is built on the [edge264-mvc](https://github.com/jens-duttke/edge264-mvc) decoder, which is linked in statically.

## Why mvc-source

- **Both views, on Linux** - a 3D Blu-ray can be decoded, filtered (e.g. frame-interpolated with [vs-rife](https://github.com/HolyWu/vs-rife)) and re-encoded as 3D without leaving Linux.
- **Exact output** - the frames are identical to those of edge264-mvc itself, which CI checks through a real `vspipe` and a real AviSynth+, and a seek gives exactly the frames of a decode in order.
- **Fast seeking** - a seek starts decoding at the nearest IDR or recovery point, the entry points a 3D Blu-ray uses, and a frame cache serves backward and repeated access from memory. On a 3D Blu-ray film, a cold seek to the last frame takes about 0.2 s.
- **Split streams** - a disc demuxed into a separate base-view `.264` and dependent-view `.mvc` (as tsMuxeR and BD3D2MK3D produce) is decoded directly, without remuxing the two files into one.
- **Quick reopening** - the first open scans the whole stream to count its frames; the result is cached in a small `.mvcidx` file next to the source, so later opens start at once.

## Installation

Prebuilt binaries for Linux (x86-64) and Windows (x64) are attached to every [release](https://github.com/jens-duttke/mvc-source/releases):

| File | Host | Platform |
|---|---|---|
| `libvsmvc.so` | VapourSynth | Linux |
| `libvsmvc.dll` | VapourSynth | Windows |
| `libavsmvc.so` | AviSynth+ | Linux |
| `libavsmvc.dll` | AviSynth+ | Windows |

The VapourSynth plugin is also available through VSRepo:

```sh
vsrepo install mvc-source
```

The binaries run on any x86-64 CPU from about 2009 onwards (`x86-64-v2`) and use AVX2 where the CPU has it.

## Usage

The input is a raw H.264 elementary stream (Annex B, usually `.264`, `.h264` or `.mvc`), not a container such as `.m2ts` or `.mkv` - demux the video stream first. It is either a single stream that carries both views, or a base-view stream plus a separate dependent-view stream passed as `dependent`.

### VapourSynth

```python
import vapoursynth as vs
core = vs.core

# both views, top-and-bottom
clip = core.mvc.Source("movie.264", stack="tab")

# a demux into two files (base view + dependent view)
clip = core.mvc.Source("movie.264", dependent="movie.mvc", stack="tab")
```

If the plugin is not installed in VapourSynth's plugin directory, load it first with `core.std.LoadPlugin("/path/to/libvsmvc.so")`.

### AviSynth+

```avisynth
LoadPlugin("/path/to/libavsmvc.so")  # libavsmvc.dll on Windows

MVCSource("movie.264", stack="tab")
MVCSource("movie.264", dependent="movie.mvc", stack="tab")
```

### Parameters

Both hosts take the same parameters:

| Parameter | Default | Meaning |
|---|---|---|
| `source` | | the H.264 stream; with `dependent`, its base view |
| `stack` | `"base"` | `"base"` (left eye / 2D), `"right"`, `"tab"` (top-and-bottom), `"sbs"` (side by side) or `"alt"` (base and dependent view alternating, twice the frames at twice the frame rate). A 2D stream returns its only view in every layout. |
| `swaplr` | off | swap the two views, for a stream authored right eye first |
| `dependent` | | the dependent-view stream of a two-file demux |
| `fpsnum`, `fpsden` | 24000 / 1001 | the frame rate, given together; it is not read from the stream yet |
| `threads` | -1 | decoding threads: -1 one per CPU core, 0 single-threaded, N exactly N |
| `cachesize` | 512 | size of the decoded-frame cache in MiB; more makes backward access smoother on streams with long GOPs |
| `showprogress` | on | report the progress of the first-open scan, through VapourSynth's log (shown by vspipe and VapourSynth Editor) or, for AviSynth+, on stderr |

The output is 8-bit YUV 4:2:0 at the full resolution of each view, so a `tab` clip of a 1080p film is 1920x2160.

## Supported streams

mvc-source decodes what edge264-mvc decodes: the **Progressive High** and **Stereo High** (MVC, 2 views) profiles of H.264, 8-bit 4:2:0, which covers 3D Blu-ray and nearly all 2D H.264 in use. Interlaced coding, higher bit depths and 4:2:2 / 4:4:4 chroma are not supported.

## Building

Building needs a C compiler and an [edge264-mvc](https://github.com/jens-duttke/edge264-mvc) source tree; pin a release tag for a reproducible build. The VapourSynth and AviSynth+ SDK headers are included, so neither host has to be installed.

```sh
make EDGE264_SRC=/path/to/edge264-mvc   # both plugins and the tests
make libvsmvc.so                        # only the VapourSynth plugin
make libavsmvc.so                       # only the AviSynth+ plugin
```

A local build targets the build machine (`-march=native`). To build the portable binaries of the releases, add `EDGE264_MAKE="VARIANTS=x86-64-v2,x86-64-v3 CFLAGS=-march=x86-64-v2"`.

### Windows

The Windows DLLs are cross-compiled on Linux with MinGW-w64 (`gcc-mingw-w64-x86-64`). The AviSynth+ plugin uses the AviSynth C interface, so the MinGW-built DLL loads in the official MSVC-built AviSynth+.

```sh
# edge264-mvc for Windows, in a separate tree
cp -r ../edge264 ../edge264-win && make -C ../edge264-win clean
make -C ../edge264-win OS=windows CC=x86_64-w64-mingw32-gcc STATIC=yes BUILDTEST=no

# the plugins
make libavsmvc.dll libvsmvc.dll CC=x86_64-w64-mingw32-gcc \
    DLLTOOL=x86_64-w64-mingw32-dlltool \
    EDGE264_SRC=../edge264-win EDGE264_MAKE="OS=windows CC=x86_64-w64-mingw32-gcc"
```

## Tests

```sh
make check TEST_FILE=movie.264            # decode core + VapourSynth plugin (mock host)
make check-bitexact TEST_FILE=movie.264   # frame checksum against edge264-mvc
make check-avs TEST_FILE=movie.264        # AviSynth+ plugin in a real AviSynth+
```

`make check` needs neither VapourSynth nor AviSynth+: it tests the decode core on all layouts (seeking must give the same frames as decoding in order, also for two-file input) and drives the VapourSynth plugin through a minimal mock host. CI additionally runs the plugin in a real VapourSynth and compares its frames with edge264-mvc, and runs the Windows build of the decode core under Wine.

## Related projects

- **[edge264-mvc](https://github.com/jens-duttke/edge264-mvc)** - the H.264 MVC decoder this plugin is built on.
- **[Oku3D Media Player](https://oku3d.com/)** - a native 3D media player built on the same decoder, which plays 3D Blu-rays and converts any 2D video to stereoscopic 3D in real time.

## Contributing

Bug reports are welcome, especially for streams that do not decode correctly. Please name the source of the stream (e.g. the disc and the demuxer used) and the parameters of the call.

## License

mvc-source is distributed under the [BSD 3-Clause license](LICENSE). It statically links edge264-mvc (BSD-3-Clause) and includes the VapourSynth SDK headers (LGPL-2.1-or-later) and the AviSynth+ headers (GPL-2.0-or-later, with the exception that permits independent plugins using only the documented interfaces).
