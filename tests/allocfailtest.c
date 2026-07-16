/*
 * allocfailtest - regression tests for decoder-allocation failure during a seek.
 *
 * A seek tears the decoder down and recreates it. If edge264_alloc fails (OOM),
 * reset_decoder returns failure - and seek_to must handle it without ever
 * decoding against the resulting NULL decoder. Two scenarios:
 *
 *   reset_retry    : an OOM during a restart-branch seek must be reported as a
 *                    clear allocation error (not a misleading end-of-stream), and
 *                    the position must stay retriable so a later request (once
 *                    memory is back) that re-enters the restart branch recovers.
 *
 *   false_restart  : after that OOM leaves the decoder NULL, a later request whose
 *                    (target, seek-point) straddle next_out - target >= next_out
 *                    and the nearest seek point <= next_out - took the NO-restart
 *                    path and decoded against the NULL decoder, surfacing the
 *                    misleading "decoder rejected input" and never re-attempting
 *                    the allocation (persistent, non-self-healing). The seek must
 *                    force a restart whenever the decoder is absent.
 *
 * edge264_alloc is intercepted via the linker's --wrap so the seek-time
 * reallocation fails deterministically, without real memory pressure.
 *
 * usage: allocfailtest <base_multigop.264>   (fixture seek points: IDRs at 0,3,6,9)
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include "mvcsource.h"
#include "edge264.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Edge264Decoder *__real_edge264_alloc(int n_threads, Edge264LogCb log_cb, void *log_arg,
	int log_mbs, Edge264AllocCb alloc_cb, Edge264FreeCb free_cb, void *alloc_arg);

static int g_fail_next_alloc = 0;

Edge264Decoder *__wrap_edge264_alloc(int n_threads, Edge264LogCb log_cb, void *log_arg,
	int log_mbs, Edge264AllocCb alloc_cb, Edge264FreeCb free_cb, void *alloc_arg) {
	if (g_fail_next_alloc) { g_fail_next_alloc = 0; return NULL; }
	return __real_edge264_alloc(n_threads, log_cb, log_arg, log_mbs, alloc_cb, free_cb, alloc_arg);
}

static uint64_t fnv_plane(const uint8_t *p, ptrdiff_t stride, int w, int h) {
	uint64_t H = 1469598103934665603ULL;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) { H ^= p[(ptrdiff_t)y * stride + x]; H *= 1099511628211ULL; }
	return H;
}

/* Fresh sequential decode of frame n's Y-plane hash (no injection). */
static int ref_hash(const char *path, int n, uint64_t *out) {
	char err[256] = "";
	MvcSource *s = mvc_open(path, 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err);
	if (!s) return -1;
	const MvcInfo *in = mvc_info(s);
	int W = in->width, H = in->height, CW = W / 2, CH = H / 2, rc = -1;
	uint8_t *Y = malloc((size_t)W * H), *U = malloc((size_t)CW * CH), *V = malloc((size_t)CW * CH);
	if (Y && U && V && !mvc_get_frame(s, n, Y, W, U, CW, V, CW, err, sizeof err)) {
		*out = fnv_plane(Y, W, W, H); rc = 0;
	}
	free(Y); free(U); free(V); mvc_close(s);
	return rc;
}

/* Scenario reset_retry (the original check): an OOM on a backward seek is reported
 * as an allocation error, and re-requesting the same frame recovers. */
static int test_reset_retry(const char *path) {
	char err[256] = "";
	MvcSource *s = mvc_open(path, 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err); /* alloc #1 succeeds */
	if (!s) { printf("FAIL[reset_retry]: open failed: %s\n", err); return 1; }
	const MvcInfo *in = mvc_info(s);
	int W = in->width, H = in->height, CW = W / 2, CH = H / 2, N = in->num_frames;
	uint8_t *Y = malloc((size_t)W * H), *U = malloc((size_t)CW * CH), *V = malloc((size_t)CW * CH);

	/* full forward decode so next_out is at the end; then a backward seek forces
	 * reset_decoder -> edge264_alloc, which we make fail. */
	if (mvc_get_frame(s, N - 1, Y, W, U, CW, V, CW, err, sizeof err)) {
		printf("FAIL[reset_retry]: read last failed: %s\n", err); mvc_close(s); free(Y); free(U); free(V); return 1;
	}

	g_fail_next_alloc = 1;
	err[0] = 0;
	int rc = mvc_get_frame(s, 1, Y, W, U, CW, V, CW, err, sizeof err); /* seek back -> alloc fails */
	int reported_alloc = rc != 0 && (strstr(err, "alloc") || strstr(err, "allocate"));
	printf("  seek-with-failed-alloc: rc=%d err=\"%s\"\n", rc, err);

	/* retry after memory is available again: the position must still be usable */
	err[0] = 0;
	int rc2 = mvc_get_frame(s, 1, Y, W, U, CW, V, CW, err, sizeof err);
	int recovered = rc2 == 0;
	printf("  retry-after-recovery: rc=%d err=\"%s\"\n", rc2, err);

	mvc_close(s); free(Y); free(U); free(V);
	if (reported_alloc && recovered) { printf("ok[reset_retry]: alloc failure reported, position recovered\n"); return 0; }
	printf("FAIL[reset_retry]: reported_alloc=%d recovered=%d\n", reported_alloc, recovered);
	return 1;
}

/* Scenario false_restart: an OOM reset leaves the decoder NULL; a later in-window
 * request must still force a restart (reallocate) rather than decode against NULL. */
static int test_false_restart_after_oom(const char *path) {
	char err[256] = "";
	uint64_t ref3;
	if (ref_hash(path, 3, &ref3)) { printf("FAIL[false_restart]: reference decode of frame 3 failed\n"); return 1; }

	MvcSource *s = mvc_open(path, 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err);
	if (!s) { printf("FAIL[false_restart]: open failed: %s\n", err); return 1; }
	const MvcInfo *in = mvc_info(s);
	int W = in->width, H = in->height, CW = W / 2, CH = H / 2, N = in->num_frames;
	if (N < 9) { printf("FAIL[false_restart]: fixture too short (%d frames, need >=9)\n", N); mvc_close(s); return 1; }
	uint8_t *Y = malloc((size_t)W * H), *U = malloc((size_t)CW * CH), *V = malloc((size_t)CW * CH);
	int rc = 1;
	if (!Y || !U || !V) { printf("FAIL[false_restart]: oom\n"); goto done; }

	/* 1. read frame 2 forward (no reset): next_out advances to 3, cache = {0,1,2} */
	if (mvc_get_frame(s, 2, Y, W, U, CW, V, CW, err, sizeof err)) { printf("FAIL[false_restart]: read 2: %s\n", err); goto done; }

	/* 2. read frame 8 with the alloc armed to fail: the seek jumps to IDR@6
	 * (sp_frame=6 > next_out=3), and its reset reallocation fails - leaving the
	 * decoder NULL and next_out at 3 (the restart aborts before updating it). */
	g_fail_next_alloc = 1;
	err[0] = 0;
	int r8 = mvc_get_frame(s, 8, Y, W, U, CW, V, CW, err, sizeof err);
	if (r8 == 0) { printf("FAIL[false_restart]: read 8 unexpectedly succeeded (no reset triggered?)\n"); goto done; }
	printf("  after-oom : read 8 rc=%d err=\"%s\"\n", r8, err);

	/* 3. read frame 3 (memory available again). target=3 >= next_out=3 and its
	 * seek point (IDR@3) <= next_out, so the old code took the no-restart path and
	 * decoded against the NULL decoder. The fix forces a restart when the decoder
	 * is absent, so this reallocates and returns the frame. */
	err[0] = 0;
	int r3 = mvc_get_frame(s, 3, Y, W, U, CW, V, CW, err, sizeof err);
	printf("  recover   : read 3 rc=%d err=\"%s\"\n", r3, err);
	if (r3 != 0) { printf("FAIL[false_restart]: decoder never recovered (decoded against NULL): %s\n", err); goto done; }
	if (fnv_plane(Y, W, W, H) != ref3) { printf("FAIL[false_restart]: recovered frame 3 has wrong content\n"); goto done; }

	rc = 0;
	printf("ok[false_restart]: seek reallocated after an OOM reset instead of decoding against NULL\n");
done:
	mvc_close(s); free(Y); free(U); free(V);
	return rc;
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: %s <base_multigop.264>\n", argv[0]); return 2; }
	int fail = 0;
	fail |= test_reset_retry(argv[1]);
	fail |= test_false_restart_after_oom(argv[1]);
	printf(fail ? "RESULT: FAIL\n" : "RESULT: PASS (alloc-failure paths never decode against a NULL decoder)\n");
	return fail ? 1 : 0;
}
