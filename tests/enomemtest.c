/*
 * enomemtest - regression test for EDGE264MVC_NOMEM handling in the decoder caller loop.
 *
 * A slice NAL whose decode returns EDGE264MVC_NOMEM produced NO frame: that is data loss,
 * not a skippable status like EDGE264MVC_UNSUPPORTED. If the core swallows it and advances, the
 * decoder yields one frame fewer than scan_index counted and every later display
 * index silently shifts. The core must instead fail loudly with a memory error.
 *
 * edge264mvc_send_nal is intercepted via the linker's --wrap so a chosen slice
 * decode returns EDGE264MVC_NOMEM deterministically, without needing real memory pressure.
 *
 * usage: enomemtest <base_multigop.264>
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include "mvcsource.h"
#include "edge264mvc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --wrap intercept: when armed, the next VCL-slice decode returns EDGE264MVC_NOMEM once. */
int __real_edge264mvc_send_nal(Edge264MvcDecoder *dec, const uint8_t *nal, size_t size, int64_t pts, int64_t user_data);

static int g_arm_enomem = 0;

int __wrap_edge264mvc_send_nal(Edge264MvcDecoder *dec, const uint8_t *nal, size_t size, int64_t pts, int64_t user_data) {
	if (g_arm_enomem && size > 0) {
		int type = nal[0] & 0x1f;
		if (type == 1 || type == 5) { g_arm_enomem = 0; return EDGE264MVC_NOMEM; }
	}
	return __real_edge264mvc_send_nal(dec, nal, size, pts, user_data);
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: %s <base_multigop.264>\n", argv[0]); return 2; }
	char err[256] = "";
	MvcSource *s = mvc_open(argv[1], 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err); /* injection off during open */
	if (!s) { fprintf(stderr, "open failed: %s\n", err); return 2; }
	const MvcInfo *in = mvc_info(s);
	int W = in->width, H = in->height, CW = W / 2, CH = H / 2, N = in->num_frames;
	uint8_t *Y = malloc((size_t)W * H), *U = malloc((size_t)CW * CH), *V = malloc((size_t)CW * CH);

	/* Arm the NOMEM injection, then read frames forward. A dropped slice must
	 * surface as a loud memory error, not a silent shift or a bare end-of-stream. */
	g_arm_enomem = 1;
	int saw_mem_error = 0, silent = 0;
	for (int i = 1; i < N; i++) {
		err[0] = 0;
		if (mvc_get_frame(s, i, Y, W, U, CW, V, CW, err, sizeof err)) {
			printf("frame %d errored: %s\n", i, err);
			if (strstr(err, "memory")) saw_mem_error = 1;
			break; /* first error stops the run */
		}
	}
	if (!saw_mem_error) silent = 1;

	mvc_close(s); free(Y); free(U); free(V);
	if (saw_mem_error) { printf("RESULT: PASS (NOMEM surfaced as a memory error)\n"); return 0; }
	printf("RESULT: FAIL (NOMEM was swallowed - no memory error reached the caller%s)\n",
	       silent ? "" : "");
	return 1;
}
