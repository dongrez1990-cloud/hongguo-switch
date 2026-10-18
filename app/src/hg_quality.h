// hg_quality.h -- the quality ladder, as pure logic.
//
// GPL-3.0-only.
//
// WHAT THE SERVICE ACTUALLY OFFERS. MEASURED against a captured response
// (app/src/tests/fixtures/video_model.json), the video model carries FIVE entries:
//
//     quality_type   definition   vwidth x vheight
//         40           360p          640x360
//         30           480p          854x480
//         20           540p         1024x576
//         10           720p         1280x720
//          1          1080p         1920x1080
//
// which is the reverse of every instinct: the LOWEST number is the HIGHEST quality, and
// 0 is never sent. Anything that sorts by quality_type ascending therefore offers the user
// 360p first and calls it "best", which looks like a working menu and is wrong. So the
// ordering lives here, where the host suite can check it against these five measured rows.
//
// WHY THE ENTRY LIST IS INDEXED RATHER THAN REBUILT. hg_play_plan_from_model picks one
// entry and builds a plan from it, discarding the rest -- correct for "play this episode",
// useless for "play it at 720p instead". The fix is to remember the ENTRY INDEX each plan
// came from, so switching quality is "build a plan from entry N of the same response"
// rather than "fetch everything again and hope the response is the same".
//
// ASCII-only source file.

#ifndef HG_QUALITY_H
#define HG_QUALITY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One selectable tier, as the parser found it. `definition` is upstream's own label
// ("720p") and is preferred for display; `label` is what to show when it is absent.
typedef struct {
  int quality_type;              // upstream's number; 0 means absent
  char definition[16];           // upstream's label, or "" when absent
  int width;
  int height;
  int bitrate;                   // 0 means absent
  long long size_bytes;          // 0 means absent
  int source_index;              // index into the parsed source list
  // THE CODEC UPSTREAM SENT FOR THIS TIER, as upstream spells it ("h264", "bytevc1", "bytevc2").
  //
  // MEASURED, and the reason this is shown rather than chosen: the codec is FIXED PER TIER. Two captures of the
  // same episode (fixtures/video_model.json and video_model_h264.json, both carrying codec_type=4 in the request
  // URL) came back as 360p=bytevc2/1080p=bytevc1 and 360p=h264/1080p=bytevc1 -- the lowest tier is the one that
  // varies between them, and no tier offers a choice. A "video codec" setting would therefore be a menu with one
  // real option, which is why C5 became "say which one you are watching" instead.
  //
  // THE NAMES ARE DELIBERATELY NOT TRANSLATED INTO FRIENDLY ONES BEYOND WHAT IS KNOWN: h264 -> "H.264" and
  // bytevc1 -> "HEVC" are established, while bytevc2 is upstream's own token and its real identity is NOT
  // measured here -- inventing a name for it would be a guess printed on screen. */
  char codec[16];
  int decodable;                 // 0 for a codec this player cannot decode
} HgQualityTier;

// Up to 16 sources is what hg_api.h allows; the ladder cannot be longer than that.
#define HG_QUALITY_MAX 16

// Build the ladder from parallel arrays, IN THE ORDER THE MENU SHOULD SHOW IT: best first,
// worst last. Entries whose codec is not decodable are included but flagged, so the menu can
// show them greyed rather than hiding them -- an absent tier reads as a bug, a greyed one
// reads as a fact.
//
// `n` is the number of entries offered. Returns the number written to `out`, which is capped
// at `cap`.
//
// The sort is by HEIGHT, descending, then by bitrate descending. Height rather than
// quality_type because quality_type's direction is an upstream convention that could change
// without notice, while "a taller picture is a better picture" is true by definition. Ties on
// height (854x480 vs 848x480) fall back to bitrate, and a tie on both falls back to the
// ORIGINAL ORDER, so the result is stable and a caller can rely on it not shuffling between
// runs.
int hg_quality_build(const int* quality_types, const char* const* definitions,
                     const int* widths, const int* heights, const int* bitrates,
                     const long long* sizes, const int* decodable, const char* const* codecs, int n,
                     HgQualityTier* out, int cap);

// The index of the tier matching `qualityType`, or -1. Used to preselect the menu on the
// tier that is currently playing -- and to notice that the current tier is not in the list
// at all, which happens when the plan was built from a bare object rather than the full
// response.
int hg_quality_index_of(const HgQualityTier* tiers, int count, int qualityType);

// The label to show for a tier. Prefers upstream's `definition`; falls back to
// "<height>p" from the measured pixel size; falls back to "quality <n>" from quality_type;
// and finally to "unknown". Never returns an empty string, because an empty menu row is
// indistinguishable from a rendering bug.
//
// Always NUL-terminates. Returns the length of the text.
int hg_quality_label(const HgQualityTier* tier, char* buf, int bufsize);

// A one-line summary of the ladder for the log: the labels from best to worst. This is the
// line that makes "the menu offers the wrong order" visible without a console run.
// Always NUL-terminates. Returns the length written.
int hg_quality_describe(const HgQualityTier* tiers, int count, char* buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif // HG_QUALITY_H
