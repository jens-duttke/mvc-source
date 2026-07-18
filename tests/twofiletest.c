/*
 * Two-file (split base + dependent view) decode test for the mvcsource core.
 *
 * A 3D Blu-ray demuxed with tsMuxeR / BD3D2MK3D yields two separate elementary
 * streams - a base-view .264 and a dependent-view .mvc - rather than the single
 * combined MVC stream mvc_open expects. mvc_open2 reads both and interleaves them
 * per access unit in memory. This test proves that path is bit-exact against the
 * already-trusted single-file (combined) decode of the very same stream:
 *
 *   1. split the combined TEST_FILE by NAL type into a base stream (the AVC NALs)
 *      and a dependent stream (the MVC-only NALs: prefix 14, subset SPS 15, coded
 *      slice extension 20) - exactly what a demuxer produces (the base is a clean
 *      2D-playable AVC stream; the MVC prefix rides with the dependent view);
 *   2. for every layout, open the combined file (mvc_open) and the split pair
 *      (mvc_open2) and require identical stream info and byte-identical frames;
 *   3. re-read frame 0 after touching the last frame, so the two-file path's
 *      backward seek (IDR re-decode) is exercised and must still match.
 *
 * usage: twofiletest <combined.264> <base_out.264> <dep_out.mvc>
 */
#include "mvcsource.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The MVC-only NAL types: prefix NAL (14), subset SPS (15), coded slice
 * extension (20). Everything else is the base (AVC) view. */
static int is_dep_type(int type) { return type == 14 || type == 15 || type == 20; }

/* Split `in` into two Annex-B files by NAL type, preserving each NAL's bytes
 * (start code included) so concatenation reproduces the stream. Returns 0, or -1
 * on any I/O error. */
static int split_file(const char *in, const char *base_out, const char *dep_out) {
	FILE *f = fopen(in, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", in); return -1; }
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
	long sz = ftell(f);
	if (sz < 4) { fprintf(stderr, "%s too short\n", in); fclose(f); return -1; }
	rewind(f);
	uint8_t *buf = malloc((size_t)sz);
	if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
	fclose(f);
	size_t n = (size_t)sz;

	FILE *fb = fopen(base_out, "wb"), *fd = fopen(dep_out, "wb");
	if (!fb || !fd) { if (fb) fclose(fb); if (fd) fclose(fd); free(buf); return -1; }

	/* first start code */
	size_t p = 0;
	while (p + 2 < n && !(buf[p] == 0 && buf[p + 1] == 0 && buf[p + 2] == 1)) p++;
	int rc = 0;
	while (p + 2 < n) { /* p sits on a 00 00 01 start code */
		size_t q = p + 3;
		while (q + 2 < n && !(buf[q] == 0 && buf[q + 1] == 0 && buf[q + 2] == 1)) q++;
		size_t chunk_end = (q + 2 < n) ? q : n; /* [p, chunk_end) = this NAL incl. start code */
		int type = buf[p + 3] & 0x1f;
		FILE *dst = is_dep_type(type) ? fd : fb;
		if (fwrite(buf + p, 1, chunk_end - p, dst) != chunk_end - p) { rc = -1; break; }
		p = chunk_end;
	}
	free(buf);
	if (fclose(fb) != 0) rc = -1;
	if (fclose(fd) != 0) rc = -1;
	return rc;
}

/* Read frame `n` in layout `lay` from `s` into caller planes; return 0 on ok. */
static int get(MvcSource *s, int n, uint8_t *Y, uint8_t *U, uint8_t *V, int W, int CW) {
	char err[256] = "";
	if (mvc_get_frame(s, n, Y, W, U, CW, V, CW, err, sizeof err)) {
		fprintf(stderr, "  get_frame(%d) failed: %s\n", n, err);
		return -1;
	}
	return 0;
}

static const char *LAYOUT_NAME[] = { "base", "right", "tab", "sbs", "alt" };

static int compare_layout(const char *combined, const char *base, const char *dep, MvcLayout lay) {
	char err[256] = "";
	MvcSource *sc = mvc_open(combined, 0, lay, 0, 0, 0, 0, err, sizeof err);
	if (!sc) { fprintf(stderr, "  combined open failed: %s\n", err); return -1; }
	MvcSource *st = mvc_open2(base, dep, 0, lay, 0, 0, 0, 0, err, sizeof err);
	if (!st) { fprintf(stderr, "  two-file open failed: %s\n", err); mvc_close(sc); return -1; }

	const MvcInfo *ic = mvc_info(sc), *it = mvc_info(st);
	if (ic->num_frames != it->num_frames || ic->width != it->width ||
	    ic->height != it->height || ic->is_mvc != it->is_mvc || ic->layout != it->layout) {
		fprintf(stderr, "  info mismatch: combined %dx%d n=%d mvc=%d lay=%d vs two-file %dx%d n=%d mvc=%d lay=%d\n",
			ic->width, ic->height, ic->num_frames, ic->is_mvc, ic->layout,
			it->width, it->height, it->num_frames, it->is_mvc, it->layout);
		mvc_close(sc); mvc_close(st); return -1;
	}

	int W = ic->width, H = ic->height, CW = W / 2, CH = H / 2;
	size_t ysz = (size_t)W * H, csz = (size_t)CW * CH;
	uint8_t *cY = malloc(ysz), *cU = malloc(csz), *cV = malloc(csz);
	uint8_t *tY = malloc(ysz), *tU = malloc(csz), *tV = malloc(csz);
	int ok = 1;
	if (!cY || !cU || !cV || !tY || !tU || !tV) { fprintf(stderr, "  oom\n"); ok = 0; goto done; }

	for (int i = 0; i < ic->num_frames && ok; i++) {
		if (get(sc, i, cY, cU, cV, W, CW) || get(st, i, tY, tU, tV, W, CW)) { ok = 0; break; }
		if (memcmp(cY, tY, ysz) || memcmp(cU, tU, csz) || memcmp(cV, tV, csz)) {
			fprintf(stderr, "  [%s] frame %d MISMATCH\n", LAYOUT_NAME[lay], i);
			ok = 0;
		}
	}
	/* backward seek on the two-file source: re-read frame 0 after the last frame */
	if (ok && ic->num_frames > 1) {
		if (get(st, ic->num_frames - 1, tY, tU, tV, W, CW) ||
		    get(sc, 0, cY, cU, cV, W, CW) || get(st, 0, tY, tU, tV, W, CW)) { ok = 0; }
		else if (memcmp(cY, tY, ysz) || memcmp(cU, tU, csz) || memcmp(cV, tV, csz)) {
			fprintf(stderr, "  [%s] frame 0 after backward seek MISMATCH\n", LAYOUT_NAME[lay]);
			ok = 0;
		}
	}
	if (ok)
		printf("  [%s] %d frames bit-exact (two-file == combined) + backward seek\n",
			LAYOUT_NAME[lay], ic->num_frames);
done:
	free(cY); free(cU); free(cV); free(tY); free(tU); free(tV);
	mvc_close(sc); mvc_close(st);
	return ok ? 0 : -1;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s <combined.264> <base_out.264> <dep_out.mvc>\n", argv[0]);
		return 2;
	}
	if (split_file(argv[1], argv[2], argv[3]) < 0) {
		fprintf(stderr, "split failed\n");
		return 1;
	}

	/* A 2D-only TEST_FILE has nothing to split into a dependent view; the test is
	 * meaningful only for an MVC stream. Detect via the combined open. */
	char err[256] = "";
	MvcSource *probe = mvc_open(argv[1], 0, MVC_BASE, 0, 0, 0, 0, err, sizeof err);
	if (!probe) { fprintf(stderr, "cannot open %s: %s\n", argv[1], err); return 1; }
	int is_mvc = mvc_info(probe)->is_mvc;
	mvc_close(probe);
	if (!is_mvc) {
		printf("TEST_FILE is 2D (no dependent view) - two-file path not applicable, skipping\n");
		return 0;
	}

	int rc = 0;
	for (MvcLayout lay = MVC_BASE; lay <= MVC_ALT; lay++)
		rc |= compare_layout(argv[1], argv[2], argv[3], lay);

	printf(rc ? "RESULT: FAIL\n" : "RESULT: PASS (two-file interleaving bit-exact, all layouts)\n");
	return rc ? 1 : 0;
}
