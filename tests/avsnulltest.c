/*
 * avsnulltest - regression test for the AviSynth+ get_frame callback's handling
 * of a failed frame allocation.
 *
 * AviSynth+'s avs_new_video_frame_a returns NULL (0) when it cannot allocate the
 * frame (memory limit / OOM - realistic on 32-bit hosts with doubled-size
 * SBS/TAB frames and a tight SetMemoryMax). avs_get_write_ptr_p(NULL, ...) then
 * dereferences the NULL VideoFrame inside the host, so a glue that passes the
 * freshly allocated frame straight into avs_get_write_ptr_p crashes the host
 * instead of letting the script fail with AviSynth's own catchable error.
 *
 * This test drives the real callback (mvc_cb_get_frame) with a mocked AviSynth C
 * API in which avs_new_video_frame_a returns NULL. The mock's avs_get_write_ptr_p
 * records (rather than performs) a NULL dereference, so the defect surfaces as a
 * recorded flag instead of a segfault. The callback must instead return NULL with
 * fi->error set and never touch the NULL frame.
 *
 * The AviSynth C API on POSIX resolves to plain extern-"C" symbols, so the host
 * functions the glue calls are satisfied here by link-time stubs; the glue source
 * is #included so the static callback is reachable.
 *
 * usage: avsnulltest <file.264>
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avisynth_c.h"

/* --- observable/controllable mock state ----------------------------------- */
static int   g_alloc_null   = 0; /* make avs_new_video_frame_a return NULL     */
static int   g_wrote_null   = 0; /* set if avs_get_write_ptr_p saw a NULL frame */
static int   g_pitch_null   = 0; /* set if avs_get_pitch_p saw a NULL frame     */
static int   g_released_null= 0; /* set if avs_release_video_frame saw NULL     */
static char *g_planebuf     = NULL; /* generous scratch plane returned by stubs */
static int   g_pitch        = 0; /* stub pitch, sized so the plane buffer holds
                                    a full write even when the glue ignores the
                                    NULL frame and decodes into g_planebuf      */

/* one dummy non-NULL frame handle so a successful path has something to return */
static int   g_dummy_frame;

/* --- stubs for the real AviSynth host functions (AVSC_API entries) --------- */
AVS_VideoFrame *avs_new_video_frame_a(AVS_ScriptEnvironment *e, const AVS_VideoInfo *vi, int align) {
	(void)e; (void)vi; (void)align;
	return g_alloc_null ? NULL : (AVS_VideoFrame *)&g_dummy_frame;
}

BYTE *avs_get_write_ptr_p(const AVS_VideoFrame *p, int plane) {
	(void)plane;
	if (!p) g_wrote_null = 1; /* the exact dereference the host would crash on */
	return (BYTE *)g_planebuf;
}

int avs_get_pitch_p(const AVS_VideoFrame *p, int plane) {
	(void)plane;
	if (!p) g_pitch_null = 1;
	return g_pitch;
}

void avs_release_video_frame(AVS_VideoFrame *f) {
	if (!f) g_released_null = 1;
}

char *avs_save_string(AVS_ScriptEnvironment *e, const char *s, int length) {
	(void)e; (void)length;
	static char buf[256];
	snprintf(buf, sizeof buf, "%s", s ? s : "");
	return buf;
}

/* referenced elsewhere in the glue TU; never called by this test, but must link */
AVS_Clip *avs_new_c_filter(AVS_ScriptEnvironment *e, AVS_FilterInfo **fi, AVS_Value child, int store_child) {
	(void)e; (void)fi; (void)child; (void)store_child; return NULL;
}
void avs_release_clip(AVS_Clip *c) { (void)c; }
void avs_set_to_clip(AVS_Value *v, AVS_Clip *c) { (void)v; (void)c; }
int avs_add_function(AVS_ScriptEnvironment *e, const char *name, const char *params,
	AVS_ApplyFunc apply, void *user_data) {
	(void)e; (void)name; (void)params; (void)apply; (void)user_data; return 0;
}

/* pull in the glue so the static callback under test is reachable */
#include "avisynth_plugin.c"

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: %s <file.264>\n", argv[0]); return 2; }

	char err[256] = "";
	MvcSource *src = mvc_open(argv[1], 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err);
	if (!src) { fprintf(stderr, "open failed: %s\n", err); return 2; }
	const MvcInfo *in = mvc_info(src);
	/* stride-aligned scratch that holds a full luma-sized write, so that if the
	 * (unfixed) glue ignores the NULL frame and decodes into g_planebuf, the test
	 * records the defect instead of crashing on a stub-buffer overrun. */
	g_pitch = (in->width + 63) & ~63;
	g_planebuf = calloc((size_t)g_pitch * in->height, 1);
	if (!g_planebuf) { fprintf(stderr, "oom\n"); mvc_close(src); return 2; }

	/* build a minimal AVS_FilterInfo the way avs_new_c_filter would have, then
	 * drive the real get_frame callback exactly as AviSynth's cache would. */
	AVS_FilterInfo fi;
	memset(&fi, 0, sizeof fi);
	fi.env = (AVS_ScriptEnvironment *)0x1; /* opaque; stubs never dereference it */
	fi.vi.width = in->width;
	fi.vi.height = in->height;
	fi.vi.pixel_type = AVS_CS_YV12;
	fi.vi.num_frames = in->num_frames;
	fi.user_data = src;
	fi.error = NULL;

	/* the frame allocation fails (host OOM) -> callback must fail cleanly */
	g_alloc_null = 1;
	AVS_VideoFrame *out = mvc_cb_get_frame(&fi, 0);

	int passed = (out == NULL)          /* signalled failure to the host        */
	          && (fi.error != NULL)     /* ... with a catchable error message   */
	          && (g_wrote_null == 0)    /* never dereferenced the NULL frame     */
	          && (g_pitch_null == 0)
	          && (g_released_null == 0);/* never released a NULL frame           */

	printf("alloc-returns-null: out=%p error=%s wrote_null=%d pitch_null=%d released_null=%d\n",
		(void *)out, fi.error ? fi.error : "(none)", g_wrote_null, g_pitch_null, g_released_null);

	/* sanity: with allocation succeeding, a normal decode still works */
	g_alloc_null = g_wrote_null = g_pitch_null = g_released_null = 0;
	fi.error = NULL;
	AVS_VideoFrame *ok = mvc_cb_get_frame(&fi, 0);
	int normal_ok = (ok != NULL) && (fi.error == NULL);
	printf("alloc-succeeds: out=%p error=%s\n", (void *)ok, fi.error ? fi.error : "(none)");

	free(g_planebuf);
	mvc_close(src);

	if (passed && normal_ok) { printf("RESULT: PASS (failed frame allocation reported, NULL frame never dereferenced)\n"); return 0; }
	printf("RESULT: FAIL (passed=%d normal_ok=%d)\n", passed, normal_ok);
	return 1;
}
