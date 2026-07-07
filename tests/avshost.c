/*
 * avshost - drive the built AviSynth+ plugin (libavsmvc.so) through a real
 * AviSynth+ runtime via its C API, the counterpart to the VapourSynth vspipe run
 * (and to tests/mockhost.c for the VapourSynth glue). It LoadPlugin()s the built
 * shared object, invokes MVCSource(), verifies the reported clip properties and
 * that frames decode, cross-checks the alt/swaplr layouts through the glue, and
 * optionally dumps one frame's planar YUV (tightly packed) for an external
 * bit-exact cross-check against edge264.
 *
 * Links against the installed AviSynth+ (-lavisynth); on POSIX the C API's
 * AVSC_API entries resolve to plain extern "C" symbols.
 *
 * usage: avshost <plugin.so> <file.264> <stack> [dumpframe dumpfile]
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avisynth_c.h"

/* Invoke MVCSource(source, stack=..., [swaplr=...]) and take the resulting clip.
 * Returns the clip (caller releases with avs_release_clip) or NULL, copying any
 * error text into err. The plugin must already be loaded (load_plugin). */
static AVS_Clip *make_clip(AVS_ScriptEnvironment *env, const char *source,
	const char *stack, int swaplr, char *err, size_t errn) {
	AVS_Value vals[3];
	const char *names[3];
	int n = 0;
	vals[n] = avs_new_value_string(source); names[n] = NULL;    n++; /* positional */
	vals[n] = avs_new_value_string(stack);  names[n] = "stack"; n++;
	if (swaplr) { vals[n] = avs_new_value_bool(1); names[n] = "swaplr"; n++; }
	AVS_Value args = avs_new_value_array(vals, n);
	AVS_Value res = avs_invoke(env, "MVCSource", args, names);
	AVS_Clip *clip = NULL;
	if (avs_is_error(res)) {
		snprintf(err, errn, "%s", avs_as_error(res) ? avs_as_error(res) : "unknown error");
	} else if (!avs_is_clip(res)) {
		snprintf(err, errn, "MVCSource did not return a clip");
	} else {
		clip = avs_take_clip(res, env);
		const char *ce = avs_clip_get_error(clip);
		if (ce) { snprintf(err, errn, "%s", ce); avs_release_clip(clip); clip = NULL; }
	}
	avs_release_value(res);
	avs_release_value(args);
	return clip;
}

/* FNV-1a hash of a plane's tightly-considered pixels (row_size bytes per row, so
 * padding stride is ignored), matching mockhost.c's per-plane comparison. */
static uint64_t plane_hash(AVS_VideoFrame *f, int plane) {
	const uint8_t *p = avs_get_read_ptr_p(f, plane);
	int pitch = avs_get_pitch_p(f, plane);
	int rows = avs_get_height_p(f, plane);
	int rowsize = avs_get_row_size_p(f, plane);
	uint64_t x = 1469598103934665603ULL;
	for (int y = 0; y < rows; y++)
		for (int c = 0; c < rowsize; c++) { x ^= p[(size_t)y * pitch + c]; x *= 1099511628211ULL; }
	return x;
}

/* Hash frame n's luma, or set *ok=0 on a getFrame failure. */
static uint64_t frame_hash(AVS_Clip *clip, int n, int *ok) {
	AVS_VideoFrame *f = avs_get_frame(clip, n);
	if (!f) { *ok = 0; return 0; }
	uint64_t h = plane_hash(f, AVS_PLANAR_Y);
	avs_release_video_frame(f);
	return h;
}

/* Write frame n as tightly-packed Y, U, V (cropped dims), the same layout the
 * VapourSynth mockhost dumps and tests/mkref.sh expects, for a bit-exact md5. */
static int dump_frame(AVS_Clip *clip, int n, const char *path) {
	AVS_VideoFrame *f = avs_get_frame(clip, n);
	if (!f) return -1;
	FILE *fp = fopen(path, "wb");
	if (!fp) { avs_release_video_frame(f); return -1; }
	const int planes[3] = { AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V };
	for (int i = 0; i < 3; i++) {
		const uint8_t *p = avs_get_read_ptr_p(f, planes[i]);
		int pitch = avs_get_pitch_p(f, planes[i]);
		int rows = avs_get_height_p(f, planes[i]);
		int rowsize = avs_get_row_size_p(f, planes[i]);
		for (int y = 0; y < rows; y++) fwrite(p + (size_t)y * pitch, 1, (size_t)rowsize, fp);
	}
	fclose(fp);
	avs_release_video_frame(f);
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s <plugin.so> <file.264> <stack> [dumpframe dumpfile]\n", argv[0]);
		return 2;
	}
	const char *plugin = argv[1], *source = argv[2], *stack = argv[3];

	/* Ask for the newest interface first, falling back so the host also runs on
	 * an older AviSynth+ (create returns NULL if the runtime is older than asked). */
	AVS_ScriptEnvironment *env = NULL;
	for (int v = 10; v >= 6 && !env; v -= 2) env = avs_create_script_environment(v);
	if (!env) { fprintf(stderr, "avs_create_script_environment failed\n"); return 1; }

	/* load the plugin once (loading twice would clash on the MVCSource name) */
	AVS_Value la[1] = { avs_new_value_string(plugin) };
	AVS_Value largs = avs_new_value_array(la, 1);
	AVS_Value lres = avs_invoke(env, "LoadPlugin", largs, NULL);
	if (avs_is_error(lres)) {
		fprintf(stderr, "LoadPlugin failed: %s\n", avs_as_error(lres));
		return 1;
	}
	avs_release_value(lres);
	avs_release_value(largs);

	int fail = 0;
	char err[512];

	/* dedicated bit-exact dump mode: dump the requested frame and exit */
	if (argc >= 6) {
		AVS_Clip *clip = make_clip(env, source, stack, 0, err, sizeof err);
		if (!clip) { fprintf(stderr, "MVCSource failed: %s\n", err); return 1; }
		int n = atoi(argv[4]);
		if (dump_frame(clip, n, argv[5]) < 0) { fprintf(stderr, "dump frame %d failed\n", n); fail = 1; }
		else fprintf(stderr, "dumped frame %d to %s\n", n, argv[5]);
		avs_release_clip(clip);
		avs_delete_script_environment(env);
		return fail;
	}

	/* 1. error path: a bogus source must be reported, not silently accepted */
	AVS_Clip *bogus = make_clip(env, "/nonexistent/does-not-exist.264", "base", 0, err, sizeof err);
	if (bogus) { printf("FAIL: bogus source accepted\n"); fail = 1; avs_release_clip(bogus); }
	else printf("ok: bogus source rejected: %s\n", err);

	/* 2. open the real file in the requested layout, validate clip properties */
	AVS_Clip *clip = make_clip(env, source, stack, 0, err, sizeof err);
	if (!clip) { fprintf(stderr, "MVCSource failed: %s\n", err); return 1; }
	const AVS_VideoInfo *vi = avs_get_video_info(clip);
	printf("vi: %dx%d  frames=%d  fps=%u/%u  pixel_type=0x%x  yv12=%d\n",
		vi->width, vi->height, vi->num_frames, vi->fps_numerator, vi->fps_denominator,
		(unsigned)vi->pixel_type, avs_is_yv12(vi));
	if (vi->width <= 0 || vi->height <= 0 || vi->num_frames <= 0) { printf("FAIL: bad vi\n"); fail = 1; }
	if (!avs_is_yv12(vi)) { printf("FAIL: format is not YV12\n"); fail = 1; }

	/* 3. pull a handful of frames (front and a middle one) */
	int probe[] = { 0, 1, 2, vi->num_frames / 2, vi->num_frames - 1 };
	for (unsigned i = 0; i < sizeof probe / sizeof *probe; i++) {
		int n = probe[i];
		if (n < 0 || n >= vi->num_frames) continue;
		AVS_VideoFrame *f = avs_get_frame(clip, n);
		if (!f) { printf("FAIL: getFrame(%d): %s\n", n, avs_clip_get_error(clip)); fail = 1; continue; }
		avs_release_video_frame(f);
		printf("ok: frame %d decoded\n", n);
	}

	/* 4. layout cross-checks through the glue: base/right/tab/alt/swaplr. These
	 * re-prove, at the AviSynth boundary, what the shared core already verifies -
	 * that the glue forwards each layout's frames correctly. */
	AVS_Clip *b = make_clip(env, source, "base", 0, err, sizeof err);
	AVS_Clip *r = make_clip(env, source, "right", 0, err, sizeof err);
	AVS_Clip *t = make_clip(env, source, "tab", 0, err, sizeof err);
	AVS_Clip *a = make_clip(env, source, "alt", 0, err, sizeof err);
	AVS_Clip *bs = make_clip(env, source, "base", 1, err, sizeof err);
	AVS_Clip *rs = make_clip(env, source, "right", 1, err, sizeof err);
	if (!b || !r || !t || !a || !bs || !rs) {
		printf("FAIL: could not open all layouts: %s\n", err); fail = 1;
	} else {
		const AVS_VideoInfo *vib = avs_get_video_info(b);
		const AVS_VideoInfo *vit = avs_get_video_info(t);
		const AVS_VideoInfo *via = avs_get_video_info(a);
		int is_mvc = vit->height == 2 * vib->height; /* tab stacks base over dep */
		int ok = 1;
		uint64_t hb0 = frame_hash(b, 0, &ok), hb1 = frame_hash(b, 1, &ok);
		uint64_t hr0 = frame_hash(r, 0, &ok);
		uint64_t hbs = frame_hash(bs, 0, &ok), hrs = frame_hash(rs, 0, &ok);
		if (!ok) { printf("FAIL: getFrame during cross-check\n"); fail = 1; }
		else if (is_mvc) {
			/* right != base; alt interleaves base,right at 2x frames/rate; swaplr swaps */
			if (hr0 == hb0) { printf("FAIL: right view identical to base\n"); fail = 1; }
			if (via->num_frames != 2 * vib->num_frames) { printf("FAIL: alt frames != 2x base\n"); fail = 1; }
			if (via->fps_numerator != 2 * vib->fps_numerator || via->fps_denominator != vib->fps_denominator) {
				printf("FAIL: alt fps != 2x base\n"); fail = 1;
			}
			uint64_t ha0 = frame_hash(a, 0, &ok), ha1 = frame_hash(a, 1, &ok), ha2 = frame_hash(a, 2, &ok);
			if (!ok) { printf("FAIL: getFrame(alt)\n"); fail = 1; }
			else if (ha0 != hb0 || ha1 != hr0 || ha2 != hb1) { printf("FAIL: alt not interleaved base/right\n"); fail = 1; }
			if (hbs != hr0 || hrs != hb0) { printf("FAIL: swaplr did not swap the views\n"); fail = 1; }
			if (!fail) printf("ok: layouts (right/alt/swaplr) correct through the glue (mvc=1)\n");
		} else {
			if (via->num_frames != vib->num_frames) { printf("FAIL: 2D alt should match base\n"); fail = 1; }
			if (hbs != hb0) { printf("FAIL: 2D swaplr changed the view\n"); fail = 1; }
			if (!fail) printf("ok: 2D stream, alt/swaplr fall back to base\n");
		}
	}
	avs_release_clip(b); avs_release_clip(r); avs_release_clip(t);
	avs_release_clip(a); avs_release_clip(bs); avs_release_clip(rs);

	/* 5. an unknown stack string must be rejected, not silently degraded */
	AVS_Clip *bad = make_clip(env, source, "sbss", 0, err, sizeof err);
	if (bad) { printf("FAIL: unknown stack 'sbss' accepted\n"); fail = 1; avs_release_clip(bad); }
	else printf("ok: unknown stack rejected: %s\n", err);

	avs_release_clip(clip);
	avs_delete_script_environment(env);
	printf(fail ? "RESULT: FAIL\n" : "RESULT: PASS\n");
	return fail;
}
