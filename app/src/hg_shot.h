// hg_shot.h -- parsing and verdict logic for an mpv screenshot-raw probe.
//
// GPL-3.0-only.
//
// WHY THIS EXISTS AT ALL. Step 5 of the wiliwili alignment shipped a preview that shows the target
// TIME and not a picture, and the reason recorded there was that a picture needs a locally captured
// frame. Two ways to get one were identified:
//
//   1. mpv's `screenshot-raw` command, which hands back a pixel buffer.
//   2. a SECOND mpv render context in software mode, rendering the target frame into a small RGBA
//      buffer. The app already has the software path, so this looked the more promising of the two
//      -- but two render contexts on one mpv handle is a configuration nobody here has verified,
//      and its failure mode is a BLACK SCREEN.
//
// Route 1 was dismissed with "screenshot-raw needs a working VO, which libmpv's render API does not
// have". That was a READING of the documentation, not a measurement, and the documentation is not
// unanimous: the manual's own caveat is about colours on D3D11, and the only remotely authoritative
// discussion found is a long-standing complaint thread about getting screenshot-raw to work from a
// libmpv embedder at all (mpv-player/mpv#5128). Neither settles it for THIS build.
//
// So this module does not decide. It PARSES the reply and STATES A VERDICT, so that one run on the
// console answers a question that would otherwise be decided by another guess -- and the answer is
// cheap: a failed probe costs one log line, and a successful one costs one frame.
//
// WHAT IT IS NOT. This is not the thumbnail feature. It is the EVIDENCE that decides whether the
// thumbnail feature is one command away or needs the second render context instead.
//
// ASCII-only source file.

#ifndef HG_SHOT_H
#define HG_SHOT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// What to report about one `screenshot-raw` reply.
typedef enum {
  HG_SHOT_OK = 0,          // a usable frame: the right fields were all there
  HG_SHOT_ERR_ARGS,        // the caller passed nothing
  HG_SHOT_ERR_PARSE,       // the reply was not a JSON object with the expected shape
  HG_SHOT_ERR_NO_ELEMENTS, // image data was present but its size does not match w*h*4
  HG_SHOT_ERR_ZERO_SIZE,   // a reply that described a 0x0 image
  HG_SHOT_ERR_STRANGE      // the reply carried a "strange colors" flag (D3D11-specific, reported
                           // because it would make every pixel wrong rather than missing)
} HgShotVerdict;

typedef struct {
  int verdict;
  int width;
  int height;
  int stride;              // 0 when the reply did not say
  char format[12];         // "bgr0"/"rgb0"/"bgra"/"rgba"/... or "" when absent
  int elements;            // the decoded byte count, 0 when absent
  int declared;            // the size the fields imply (w * h * (stride ? stride / w : 4))
  // The name of the first field that was missing, for the log. Points at a string literal.
  const char *missing;
} HgShotInfo;

// Parse an mpv `screenshot-raw` reply.
//
// THE SHAPE, from mpv's manual: a map with w, h, stride, format and data, where format is one of
// bgr0/rgb0/bgra/rgba and data is a byte array of stride*h bytes. `stride` is documented as unused
// and set to 0 by some builds, which is why it is reported rather than required -- but if it IS
// present and non-zero, w * stride must match `data`'s size, and a mismatch means the fields
// disagree about the frame and nothing should be drawn from it.
//
// Returns the verdict, and fills `out` when `out` is non-NULL.
int hg_shot_parse(const char *json, size_t len, HgShotInfo *out);

// A one-line description of a verdict, for the log. Never NULL.
const char *hg_shot_verdict_text(int verdict);

// A one-line summary of the measurement, for the log: the size, the format and whether the declared
// and actual byte counts agree. This is the line that makes a failed probe diagnosable without
// another run.
int hg_shot_describe(const HgShotInfo *info, char *buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif // HG_SHOT_H
