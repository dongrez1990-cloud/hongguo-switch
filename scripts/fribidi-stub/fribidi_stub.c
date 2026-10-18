/*
 * Minimal LTR-only stand-in for GNU FriBidi.
 *
 * WHY THIS EXISTS
 * ---------------
 * The Switch build's static link chain includes -lfribidi (libass needs it for
 * bidirectional subtitle text). devkitPro's switch-libfribidi cannot be fetched
 * here -- pkg.devkitpro.org is Cloudflare-403'd from this machine -- and its
 * source recipe is autotools, which fails in this environment in a way that
 * resisted two rounds of diagnosis (aarch64-none-elf-gcc compiles the conftest
 * fine, then the same configure run probes for a bare `gcc` and gives up).
 *
 * Rather than keep fighting autotools, this measures the actual requirement:
 * `aarch64-none-elf-nm -u libass.a` shows libass needs exactly SEVEN fribidi
 * symbols, and neither libavfilter nor libharfbuzz needs any:
 *
 *     fribidi_get_bidi_types              fribidi_get_bracket_types
 *     fribidi_get_joining_types           fribidi_get_par_embedding_levels_ex
 *     fribidi_join_arabic                 fribidi_reorder_line
 *     fribidi_shape
 *
 * This file implements those seven.
 *
 * WHAT IT DOES NOT DO  (deliberate, documented degradation)
 * ---------------------------------------------------------
 * It treats every character as left-to-right: bidi types are all LTR, bracket
 * types are all "none", joining types are all "non-joining", embedding levels
 * are all 0, the visual reorder map is the identity, and shaping is a no-op.
 *
 * For left-to-right text -- which includes every Chinese and Latin subtitle --
 * this is not an approximation, it is the correct answer: no reordering is
 * required and the identity map is what real FriBidi returns too.
 *
 * For Arabic and Hebrew text it produces WRONG output: no RTL reordering, no
 * Arabic joining, no mirroring. 红果短剧 is Chinese-only, so this does not
 * affect the intended use, but it is a real functional regression relative to
 * real FriBidi and must be recorded as such. Replace this with a genuine build
 * of FriBidi 1.0.12 (recipe: devkitPro/pacman-packages switch/libfribidi,
 * reproduced in scripts/build-missing-portlibs.sh stage 3) before shipping to
 * anyone who needs RTL subtitle support.
 *
 * The declarations below are checked against the real FriBidi headers, which
 * the build installs into the portlibs include dir, so any signature drift
 * fails the build instead of silently corrupting the ABI.
 */

#include <fribidi/fribidi.h>

FRIBIDI_ENTRY void
fribidi_get_bidi_types (const FriBidiChar *str,
                        const FriBidiStrIndex len,
                        FriBidiCharType *btypes)
{
  FriBidiStrIndex i;
  (void) str;                   /* contents are irrelevant: everything is LTR */
  for (i = 0; i < len; i++)
    btypes[i] = FRIBIDI_TYPE_LTR;
}

FRIBIDI_ENTRY void
fribidi_get_bracket_types (const FriBidiChar *str,
                           const FriBidiStrIndex len,
                           const FriBidiCharType *types,
                           FriBidiBracketType *btypes)
{
  FriBidiStrIndex i;
  (void) str;
  (void) types;
  /* 0 is the "no bracket" enumerator in FriBidi's FriBidiBracketType; using the
   * literal keeps this compiling across the 1.0.x renames (NONE vs NO_BRACKET). */
  for (i = 0; i < len; i++)
    btypes[i] = (FriBidiBracketType) 0;
}

FRIBIDI_ENTRY void
fribidi_get_joining_types (const FriBidiChar *str,
                           const FriBidiStrIndex len,
                           FriBidiJoiningType *jtypes)
{
  FriBidiStrIndex i;
  (void) str;
  for (i = 0; i < len; i++)
    jtypes[i] = FRIBIDI_JOINING_TYPE_U;   /* U = non-joining */
}

FRIBIDI_ENTRY FriBidiLevel
fribidi_get_par_embedding_levels_ex (const FriBidiCharType *bidi_types,
                                     const FriBidiBracketType *bracket_types,
                                     const FriBidiStrIndex len,
                                     FriBidiParType *pbase_dir,
                                     FriBidiLevel *embedding_levels)
{
  FriBidiStrIndex i;
  (void) bidi_types;
  (void) bracket_types;
  for (i = 0; i < len; i++)
    embedding_levels[i] = 0;            /* level 0 == LTR */
  if (pbase_dir)
    *pbase_dir = FRIBIDI_PAR_LTR;       /* resolved base direction */
  return 0;                             /* FriBidiLevel: 0 means success */
}

FRIBIDI_ENTRY void
fribidi_join_arabic (const FriBidiCharType *bidi_types,
                     const FriBidiStrIndex len,
                     const FriBidiLevel *embedding_levels,
                     FriBidiArabicProp *ar_props)
{
  (void) bidi_types;
  (void) len;
  (void) embedding_levels;
  (void) ar_props;                      /* nothing joins when nothing is Arabic */
}

FRIBIDI_ENTRY FriBidiLevel
fribidi_reorder_line (FriBidiFlags flags,
                      const FriBidiCharType *bidi_types,
                      const FriBidiStrIndex len,
                      const FriBidiStrIndex off,
                      const FriBidiParType base_dir,
                      FriBidiLevel *embedding_levels,
                      FriBidiChar *visual_str,
                      FriBidiStrIndex *map)
{
  FriBidiStrIndex i;
  (void) flags;
  (void) bidi_types;
  (void) off;
  (void) base_dir;
  (void) embedding_levels;
  (void) visual_str;                    /* logical order already == visual order */
  if (map)
    for (i = 0; i < len; i++)
      map[i] = i;                       /* identity permutation */
  return 0;
}

FRIBIDI_ENTRY void
fribidi_shape (FriBidiFlags flags,
               const FriBidiLevel *embedding_levels,
               const FriBidiStrIndex len,
               FriBidiArabicProp *ar_props,
               FriBidiChar *str)
{
  (void) flags;
  (void) embedding_levels;
  (void) len;
  (void) ar_props;
  (void) str;                           /* no Arabic shaping to apply */
}
