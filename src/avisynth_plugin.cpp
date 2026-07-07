/*
 * avisynth-mvc - AviSynth+ source filter exposing edge264-mvc as MVCSource().
 * Thin C++ glue over the decode core in mvcsource.c (the same core the
 * VapourSynth plugin uses); all the real logic lives in the core.
 *
 * Copyright (c) 2026 Jens Duttke. BSD-3-Clause (see LICENSE).
 */
#include <cstdint>
#include <cstring>
#include <strings.h>

#include "avisynth.h"

extern "C" {
#include "mvcsource.h" /* C core: no C++ linkage guards of its own */
}

/* Returns a layout (0..4), or -1 for an unrecognized string so the caller can
 * report it instead of silently degrading a typo to the base (mono) view. Same
 * spellings as the VapourSynth plugin's parse_layout; an unset stack defaults to
 * base (handled by the AsString default in Create_MVCSource). */
static int parse_layout(const char *s) {
	if (!s || !strcasecmp(s, "base") || !strcasecmp(s, "left") || !strcasecmp(s, "l")) return MVC_BASE;
	if (!strcasecmp(s, "right") || !strcasecmp(s, "r")) return MVC_RIGHT;
	if (!strcasecmp(s, "tab") || !strcasecmp(s, "tb") || !strcasecmp(s, "topbottom")) return MVC_TAB;
	if (!strcasecmp(s, "sbs") || !strcasecmp(s, "lr") || !strcasecmp(s, "sidebyside")) return MVC_SBS;
	if (!strcasecmp(s, "alt") || !strcasecmp(s, "alternate") || !strcasecmp(s, "alternating")) return MVC_ALT;
	return -1; /* unknown */
}

/* IClip wrapper: owns the decode core and serves each output frame in the
 * configured layout. The core mutates internal state per request (file position,
 * decoder, seek-by-reset), so the filter reports MT_SERIALIZED - AviSynth+ then
 * serialises GetFrame for this instance, the counterpart to VapourSynth's
 * fmUnordered. */
class MVCSourceClip : public IClip {
	MvcSource *src_;
	VideoInfo vi_;
public:
	MVCSourceClip(MvcSource *src, const MvcInfo *info) : src_(src) {
		memset(&vi_, 0, sizeof vi_);
		vi_.width = info->width;
		vi_.height = info->height;
		vi_.num_frames = info->num_frames;
		vi_.pixel_type = VideoInfo::CS_YV12; /* YUV420P8, matching the core's output */
		vi_.audio_samples_per_second = 0;    /* no audio */
		/* SetFPS reduces the fraction to lowest terms itself (unlike VapourSynth,
		 * where the plugin must reduce). The core's rates fit in unsigned (the
		 * alt layout at most doubles fps_num, e.g. 24000 -> 48000). */
		vi_.SetFPS((unsigned)info->fps_num, (unsigned)info->fps_den);
	}
	~MVCSourceClip() { mvc_close(src_); }

	PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment *env) override {
		PVideoFrame frame = env->NewVideoFrame(vi_);
		char err[256];
		/* PLANAR_U/PLANAR_V give the logical U/V plane pointers; AviSynth's YV12
		 * stores V before U physically, but the plane constants hide that, so no
		 * manual swap is needed - the core writes U into U and V into V. */
		if (mvc_get_frame(src_, n,
				frame->GetWritePtr(PLANAR_Y), frame->GetPitch(PLANAR_Y),
				frame->GetWritePtr(PLANAR_U), frame->GetPitch(PLANAR_U),
				frame->GetWritePtr(PLANAR_V), frame->GetPitch(PLANAR_V),
				err, sizeof err)) {
			env->ThrowError("MVCSource: %s", err);
		}
		return frame;
	}

	bool __stdcall GetParity(int n) override { (void)n; return true; } /* progressive */
	void __stdcall GetAudio(void *buf, int64_t start, int64_t count, IScriptEnvironment *env) override {
		(void)buf; (void)start; (void)count; (void)env; /* no audio */
	}
	int __stdcall SetCacheHints(int cachehints, int frame_range) override {
		(void)frame_range;
		return cachehints == CACHE_GET_MTMODE ? MT_SERIALIZED : 0;
	}
	const VideoInfo &__stdcall GetVideoInfo() override { return vi_; }
};

/* MVCSource(source[, stack, threads, fpsnum, fpsden, swaplr]). Argument names and
 * semantics mirror the VapourSynth plugin so scripts read the same across hosts. */
static AVSValue __cdecl Create_MVCSource(AVSValue args, void *user_data, IScriptEnvironment *env) {
	(void)user_data;
	if (!args[0].Defined())
		env->ThrowError("MVCSource: 'source' (path to an .264/.h264 Annex-B file) is required");
	const char *source = args[0].AsString();
	const char *stack = args[1].AsString("base");
	int layout = parse_layout(stack);
	if (layout < 0)
		env->ThrowError("MVCSource: unknown stack mode '%s' (use base/right/tab/sbs/alt)", stack);
	int threads = args[2].AsInt(0);
	/* fpsnum/fpsden are a pair: reject a half-specified or non-positive rate
	 * rather than splice a user value with half of the default. Only both-absent
	 * reaches mvc_open as 0/0, which then keeps its 24000/1001 default. */
	bool have_num = args[3].Defined();
	bool have_den = args[4].Defined();
	if (have_num != have_den)
		env->ThrowError("MVCSource: fpsnum and fpsden must be specified together");
	int64_t fpsnum = have_num ? (int64_t)args[3].AsInt() : 0;
	int64_t fpsden = have_den ? (int64_t)args[4].AsInt() : 0;
	if ((have_num && fpsnum <= 0) || (have_den && fpsden <= 0))
		env->ThrowError("MVCSource: fpsnum and fpsden must be positive");
	int swaplr = args[5].AsBool(false) ? 1 : 0;

	char emsg[256];
	MvcSource *src = mvc_open(source, threads, (MvcLayout)layout, swaplr, fpsnum, fpsden, emsg, sizeof emsg);
	if (!src)
		env->ThrowError("MVCSource: %s", emsg);
	return new MVCSourceClip(src, mvc_info(src));
}

/* AvisynthPluginInit3 must stay visible under -fvisibility=hidden (the build
 * hides everything else, including edge264's statically-linked symbols), just as
 * the VapourSynth glue exports only VapourSynthPluginInit2. __stdcall is empty on
 * POSIX (avs/posix.h). */
#if defined(_WIN32) || defined(_WIN64)
	#define MVC_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
	#define MVC_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

const AVS_Linkage *AVS_linkage = nullptr;

MVC_PLUGIN_EXPORT const char *__stdcall
AvisynthPluginInit3(IScriptEnvironment *env, const AVS_Linkage *const vectors) {
	AVS_linkage = vectors;
	/* [source] positional-or-named; the rest named-optional, matching the
	 * VapourSynth argument set (swaplr last). */
	env->AddFunction("MVCSource",
		"[source]s[stack]s[threads]i[fpsnum]i[fpsden]i[swaplr]b",
		Create_MVCSource, 0);
	return "MVCSource: H.264 MVC (3D) and AVC source, built on edge264-mvc";
}
