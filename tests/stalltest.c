/*
 * stalltest - regression test for the progress guard in the decoder caller loop
 * (decode_next_output).
 *
 * A crafted / corrupt .264 could drive the decoder into a state where its output
 * queue is full of never-completed pictures: every NAL sent - and the end of the
 * stream too - is answered with "receive frames first" (EDGE264MVC_AGAIN under
 * API v2, ENOBUFS under the edge264 API), while receiving drains nothing. A caller
 * loop that simply resends spins forever at ~100% CPU: a source filter for
 * untrusted media that can be driven into a hang is a DoS.
 *
 * Both decoder entry points are intercepted via the linker's --wrap to reproduce
 * the stuck state deterministically: real decode during open, then persistent
 * EDGE264MVC_AGAIN with no frame emitted. The caller loop must terminate with a
 * clear error instead of spinning. A high escape threshold in the send wrap makes
 * an unguarded (spinning) loop still return, so this test itself never hangs.
 *
 * usage: stalltest <base_multigop.264>
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include "mvcsource.h"
#include "edge264mvc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int __real_edge264mvc_send_nal(Edge264MvcDecoder *dec, const uint8_t *nal, size_t size, int64_t pts, int64_t user_data);
int __real_edge264mvc_receive_frame(Edge264MvcDecoder *dec, Edge264MvcFrame *out);

/* When armed, every send_nal returns EDGE264MVC_AGAIN and every receive_frame
 * gives nothing, standing in for a DPB stuck full of incomplete pictures. The
 * escape net turns AGAIN into a fatal EDGE264MVC_INVALID after ESCAPE calls, so a still-spinning (unfixed)
 * loop terminates the test instead of hanging it - the call count then reveals
 * whether the loop bailed at its guard (~65) or spun to the net. */
#define ESCAPE 4000
static int  g_stuck = 0;
static long g_stuck_calls = 0;

int __wrap_edge264mvc_send_nal(Edge264MvcDecoder *dec, const uint8_t *nal, size_t size, int64_t pts, int64_t user_data) {
	if (g_stuck) {
		if (++g_stuck_calls > ESCAPE) return EDGE264MVC_INVALID; /* safety net: never hang the test */
		return EDGE264MVC_AGAIN;
	}
	return __real_edge264mvc_send_nal(dec, nal, size, pts, user_data);
}

int __wrap_edge264mvc_receive_frame(Edge264MvcDecoder *dec, Edge264MvcFrame *out) {
	if (g_stuck) return EDGE264MVC_AGAIN; /* no complete picture available to emit */
	return __real_edge264mvc_receive_frame(dec, out);
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: %s <base_multigop.264>\n", argv[0]); return 2; }
	char err[256] = "";
	MvcSource *s = mvc_open(argv[1], 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err); /* injection off during open */
	if (!s) { fprintf(stderr, "open failed: %s\n", err); return 2; }
	const MvcInfo *in = mvc_info(s);
	int W = in->width, H = in->height, CW = W / 2, CH = H / 2, N = in->num_frames;
	uint8_t *Y = malloc((size_t)W * H), *U = malloc((size_t)CW * CH), *V = malloc((size_t)CW * CH);
	if (!Y || !U || !V) { fprintf(stderr, "oom\n"); return 2; }

	/* Frame 1 is uncached (open cached only frame 0) and needs a decode, so this
	 * request enters the caller loop. With the stuck injection armed, the loop
	 * must not spin: it has to return an error within its progress guard. */
	int target = N > 1 ? 1 : 0;
	g_stuck = 1;
	g_stuck_calls = 0;
	err[0] = 0;
	int rc = mvc_get_frame(s, target, Y, W, U, CW, V, CW, err, sizeof err);

	int errored = rc != 0;                 /* the loop gave up loudly ...          */
	int hung    = g_stuck_calls > 500;     /* ... rather than spinning to the net  */
	printf("stuck-decode: rc=%d calls=%ld err=\"%s\"\n", rc, g_stuck_calls, err);

	mvc_close(s); free(Y); free(U); free(V);
	if (errored && !hung) { printf("RESULT: PASS (caller loop bailed on a persistent AGAIN stall, no spin)\n"); return 0; }
	printf("RESULT: FAIL (errored=%d hung=%d calls=%ld)\n", errored, hung, g_stuck_calls);
	return 1;
}
