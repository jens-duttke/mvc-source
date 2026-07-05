#!/usr/bin/env python3
"""Exercise tests/test.vpy's argument guards without a real VapourSynth runtime.

vspipe injects each --arg as a module global before running the script, so a
mistyped --arg *name* (stak=sbs) silently vanishes into the script default and a
misspelled stack *value* (top-bottom) used to degrade to the base view. The
script must reject an unknown stack value and warn about unexpected args.

A stub `vapoursynth` module is installed so test.vpy can be exec'd with a chosen
set of injected globals, exactly as vspipe would provide them.
"""
import io
import sys
import types
import contextlib

VPY = "tests/test.vpy"


def make_stub():
    vs = types.ModuleType("vapoursynth")

    class _Fmt:
        name = "YUV420P8"

    class _Clip:
        width, height, num_frames = 640, 480, 9
        fps_num, fps_den = 24000, 1001
        format = _Fmt()

        def set_output(self, index):
            pass

    class _NS:
        def __getattr__(self, name):
            def call(*a, **k):
                return _Clip() if name == "Source" else None
            return call

    class _Core:
        def __getattr__(self, name):
            return _NS()

    vs.core = _Core()
    return vs


def run_vpy(argmap):
    """exec test.vpy with `argmap` injected as globals; return captured stderr."""
    sys.modules["vapoursynth"] = make_stub()
    g = {"__name__": "__main__"}
    g.update(argmap)
    with open(VPY, "r", encoding="utf-8") as f:
        code = compile(f.read(), VPY, "exec")
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        exec(code, g)
    return err.getvalue()


def expect_raise(argmap, label):
    try:
        run_vpy(dict(argmap))
    except Exception as e:  # noqa: BLE001 - any raise is the point
        print(f"ok[vpy]: {label} -> raised {type(e).__name__}: {e}")
        return 0
    print(f"FAIL[vpy]: {label} did not raise")
    return 1


def expect_ok(argmap, label):
    try:
        run_vpy(dict(argmap))
        print(f"ok[vpy]: {label} ran")
        return 0
    except Exception as e:  # noqa: BLE001
        print(f"FAIL[vpy]: {label} unexpectedly raised {type(e).__name__}: {e}")
        return 1


def expect_warn(argmap, needle, label):
    try:
        out = run_vpy(dict(argmap))
    except Exception as e:  # noqa: BLE001
        print(f"FAIL[vpy]: {label} raised instead of warning: {e}")
        return 1
    if needle in out:
        print(f"ok[vpy]: {label} warned ({needle!r} in stderr)")
        return 0
    print(f"FAIL[vpy]: {label} did not warn about {needle!r}")
    return 1


def check_doc():
    """The header's smoke-test example pipes vspipe into `ffmpeg -i -`, which
    needs a container to probe; vspipe emits raw planar output unless -c y4m is
    given, so the documented command must include it."""
    with open(VPY, "r", encoding="utf-8") as f:
        header = f.read().split("\nimport ", 1)[0]
    if "ffmpeg -i -" not in header:
        print("ok[vpy]: header has no 'ffmpeg -i -' pipe to lint")
        return 0
    if "-c y4m" in header:
        print("ok[vpy]: header example uses -c y4m for the ffmpeg pipe")
        return 0
    print("FAIL[vpy]: header pipes vspipe into 'ffmpeg -i -' without -c y4m")
    return 1


def main():
    fail = 0
    fail |= check_doc()
    fail |= expect_ok({"source": "x.264", "stack": "tab"}, "valid stack=tab")
    fail |= expect_ok({"source": "x.264", "stack": "base"}, "valid stack=base")
    fail |= expect_raise({"source": "x.264", "stack": "top-bottom"}, "bad stack value")
    fail |= expect_raise({"source": "x.264", "stack": "sbss"}, "bad stack value (sbss)")
    fail |= expect_warn({"source": "x.264", "stak": "sbs"}, "stak", "typo'd --arg name")
    print("RESULT:", "FAIL" if fail else "PASS")
    return fail


if __name__ == "__main__":
    sys.exit(main())
