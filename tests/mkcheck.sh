#!/bin/sh
# Build-system / CI-config checks that a compiled test cannot express.
#
#   M6  $(EDGE264_A) must always be delegated to the edge264 sub-make (via a
#       FORCE prerequisite), so a changed edge264 tree is never linked as a stale
#       static lib. A file target with no prerequisites is treated as up to date
#       once it exists, so without FORCE the sub-make never re-runs.
#   M7  the CI must pin EDGE264_REF to a release tag, not a floating branch.
#   L12 the CI must pin the VapourSynth clone to a release tag, not master.
#
# usage: sh tests/mkcheck.sh [EDGE264_SRC]
EDGE264_SRC="${1:-${EDGE264_SRC:-../edge264}}"
WF=".github/workflows/tests.yml"
PLUGIN_SO="libvsmvc.so"
fail=0

# --- M6: edge264 sub-make delegation --------------------------------------
A="$EDGE264_SRC/libedge264.a"
if [ ! -f "$A" ]; then
	make EDGE264_SRC="$EDGE264_SRC" "$A" >/dev/null 2>&1 || true
fi
if make -n EDGE264_SRC="$EDGE264_SRC" coretest 2>/dev/null | grep -q "STATIC=yes"; then
	echo "ok[makefile]: edge264 sub-make is always delegated (no stale .a)"
else
	echo "FAIL[makefile]: a stale libedge264.a would be linked (sub-make not re-run when .a exists)"
	fail=1
fi

# --- L7: -fPIC survives a CFLAGS override ---------------------------------
# The shared object needs -fPIC; it must live in the rule, not the overridable
# CFLAGS default. Pretend plugin.c changed (-W) so the recipe prints, override
# CFLAGS, and check the plugin compile line still carries -fPIC.
if make -n -W src/plugin.c CFLAGS="-O3" EDGE264_SRC="$EDGE264_SRC" "$PLUGIN_SO" 2>/dev/null \
	| grep 'fvisibility=hidden' | grep -q -- '-fPIC'; then
	echo "ok[makefile]: -fPIC kept when CFLAGS is overridden"
else
	echo "FAIL[makefile]: -fPIC dropped when CFLAGS is overridden (move it into the \$(PLUGIN) rule)"
	fail=1
fi

# --- N1: no unguarded `s->nal = e + 3` (would form s->end + 3, formal UB) --
# Behaviour-preserving fix, so there is no runtime red/green: guard structurally
# that the pattern stays in the `(e < s->end) ? e + 3 : s->end` form.
if grep -nE 's->nal[[:space:]]*=[[:space:]]*e[[:space:]]*\+[[:space:]]*3[[:space:]]*;' src/mvcsource.c >/dev/null 2>&1; then
	echo "FAIL[source]: unguarded 's->nal = e + 3' in mvcsource.c (can form s->end + 3, UB)"
	fail=1
else
	echo "ok[source]: decode_next_output guards e + 3 against s->end"
fi

# --- L9: no fixed /tmp reference names (per-run mktemp instead) ------------
if grep -q '/tmp/vsedge' Makefile 2>/dev/null; then
	echo "FAIL[makefile]: check-bitexact uses fixed /tmp names (clobber/symlink surface; use mktemp)"
	fail=1
else
	echo "ok[makefile]: check-bitexact uses per-run temp files"
fi

# --- L10: check-bitexact guards TEST_FILE like check does -----------------
if [ "$(grep -c 'ifndef TEST_FILE' Makefile)" -ge 2 ]; then
	echo "ok[makefile]: check-bitexact guards TEST_FILE"
else
	echo "FAIL[makefile]: check-bitexact lacks the TEST_FILE guard"
	fail=1
fi

# --- L11: the reference extraction is shared (tests/mkref.sh) --------------
if grep -q 'mkref.sh' Makefile 2>/dev/null && grep -q 'mkref.sh' "$WF" 2>/dev/null; then
	echo "ok[ci]: bit-exact reference extraction shared via tests/mkref.sh"
else
	echo "FAIL[ci]: bit-exact reference extraction duplicated (factor into tests/mkref.sh)"
	fail=1
fi

# --- M7: EDGE264_REF pinned to a tag --------------------------------------
ref=$(grep -E '^[[:space:]]*EDGE264_REF:' "$WF" 2>/dev/null | head -1 | sed -E 's/.*EDGE264_REF:[[:space:]]*//; s/[[:space:]]*$//')
case "$ref" in
	main|master|"") echo "FAIL[ci]: EDGE264_REF is '${ref:-unset}' (pin a release tag for reproducibility)"; fail=1;;
	*) echo "ok[ci]: EDGE264_REF pinned to '$ref'";;
esac

# --- L12: VapourSynth clone pinned to a tag -------------------------------
vsline=$(grep -E 'clone .*github.com/vapoursynth/vapoursynth' "$WF" 2>/dev/null | head -1)
if [ -z "$vsline" ]; then
	echo "ok[ci]: no VapourSynth clone found"
elif echo "$vsline" | grep -q -- '--branch'; then
	echo "ok[ci]: VapourSynth clone pinned to a release tag"
else
	echo "FAIL[ci]: VapourSynth is cloned from an unpinned default branch (add --branch R<release>)"
	fail=1
fi

[ "$fail" -eq 0 ] && echo "mkcheck: PASS" || echo "mkcheck: FAIL"
exit "$fail"
