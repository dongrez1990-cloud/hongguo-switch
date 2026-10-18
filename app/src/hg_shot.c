// hg_shot implementation. See hg_shot.h for why this probe exists.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "hg_shot.h"

#include "hg_router.h"   // hg_json_object_get / hg_json_int / hg_json_array_first / hg_json_string

#include <stdio.h>
#include <string.h>

static void putStr(char *dst, int cap, const char *src) {
  int i = 0;
  if (!dst || cap <= 0) return;
  if (!src) src = "";
  while (src[i] != '\0' && i < cap - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

/* A JSON array's byte length: [1,2,3] is 6 bytes of data. Walks the array rather than counting
 * commas, because the values can be any width and a comma count is off by one for an empty array.
 *
 * Returns -1 when the array cannot be walked, which is a malformed reply rather than an empty one. */
static int arrayByteCount(const char *j, size_t len, size_t arr, int *outCount) {
  size_t elem = 0;
  int n = 0;
  if (!j || !outCount) return -1;
  if (!hg_json_array_first(j, len, arr, &elem)) {
    /* An empty array is legal and means zero bytes. */
    *outCount = 0;
    return 0;
  }
  for (;;) {
    long long v = 0;
    if (!hg_json_int(j, len, elem, &v)) return -1;
    n++;
    {
      size_t next;
      if (!hg_json_array_next(j, len, arr, elem, &next)) break;
      elem = next;
    }
  }
  *outCount = n;
  return 0;
}

int hg_shot_parse(const char *json, size_t len, HgShotInfo *out) {
  HgShotInfo info;
  size_t v = 0;
  long long w = 0, h = 0, stride = 0;
  int haveW = 0, haveH = 0, haveData = 0;

  memset(&info, 0, sizeof info);
  info.verdict = HG_SHOT_OK;
  if (!json || len == 0) {
    info.verdict = HG_SHOT_ERR_ARGS;
    if (out) *out = info;
    return info.verdict;
  }

  /* The reply must be an object; anything else is a shape this probe does not know, and saying
   * "parse failed" is more useful than reporting five zeroes as if they were measurements. */
  {
    size_t p = 0;
    while (p < len && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) p++;
    if (p >= len || json[p] != '{') {
      info.verdict = HG_SHOT_ERR_PARSE;
      info.missing = "(reply is not an object)";
      if (out) *out = info;
      return info.verdict;
    }
  }

  /* ---- w and h, which are required ---------------------------------------- */
  if (hg_json_object_get(json, len, 0, "w", &v) && hg_json_int(json, len, v, &w)) {
    info.width = (int)w;
    haveW = 1;
  } else {
    info.missing = "w";
  }
  if (hg_json_object_get(json, len, 0, "h", &v) && hg_json_int(json, len, v, &h)) {
    info.height = (int)h;
    haveH = 1;
  } else if (!info.missing) {
    info.missing = "h";
  }

  /* ---- the optional fields, reported rather than required ------------------ */
  if (hg_json_object_get(json, len, 0, "stride", &v) && hg_json_int(json, len, v, &stride)) {
    /* mpv documents stride as unused and set to 0; a build that fills it in gets checked below. */
    info.stride = (int)stride;
  }
  if (hg_json_object_get(json, len, 0, "format", &v)) {
    if (hg_json_string(json, len, v, info.format, (int)sizeof info.format) < 0) {
      info.format[0] = '\0';
    }
  }
  /* A "strange colors" flag would make every pixel wrong rather than absent, which is a different
   * problem from a missing frame -- so it is reported as its own verdict instead of being ignored. */
  {
    long long strange = 0;
    if (hg_json_object_get(json, len, 0, "strange_colors", &v) && hg_json_int(json, len, v, &strange) &&
        strange != 0) {
      info.verdict = HG_SHOT_ERR_STRANGE;
    }
  }

  /* ---- the pixels --------------------------------------------------------- */
  if (hg_json_object_get(json, len, 0, "data", &v)) {
    size_t probe = v;
    while (probe < len && (json[probe] == ' ' || json[probe] == '\t')) probe++;
    if (probe < len && json[probe] == '[') {
      int count = 0;
      if (arrayByteCount(json, len, v, &count) == 0) {
        info.elements = count;
        haveData = 1;
      }
    }
  }
  if (!haveData && !info.missing) info.missing = "data";

  /* The declared size, computed as mpv's own convention: stride when it is known and non-zero,
   * otherwise four bytes per pixel. bgr0/rgb0/bgra/rgba are all 4 bytes per pixel, which is why
   * this is safe without consulting `format`. */
  if (haveW && haveH && info.width > 0 && info.height > 0) {
    int perRow = (info.stride > 0 && info.width > 0 && info.stride >= info.width * 4)
                     ? info.stride
                     : info.width * 4;
    info.declared = perRow * info.height;
  }

  if (info.verdict != HG_SHOT_OK) {
    if (out) *out = info;
    return info.verdict;
  }

  /* ORDER MATTERS. "Not an object" is a different fact from "the fields are missing", and both are
   * different from "the fields contradict each other". */
  if (!haveW || !haveH || !haveData) {
    info.verdict = HG_SHOT_ERR_PARSE;
    if (out) *out = info;
    return info.verdict;
  }
  if (info.width <= 0 || info.height <= 0) {
    info.verdict = HG_SHOT_ERR_ZERO_SIZE;
    if (out) *out = info;
    return info.verdict;
  }
  /* THE CONSISTENCY CHECK, and the reason this is worth parsing at all rather than just checking
   * that the command returned success: a reply whose `data` does not match its own w/h/stride
   * would produce a frame with the wrong stride, and a thumbnail drawn from it would be sheared
   * diagonally -- which looks like a rendering bug in nanovg rather than a bad reply. */
  if (info.elements != info.declared) {
    info.verdict = HG_SHOT_ERR_NO_ELEMENTS;
    if (out) *out = info;
    return info.verdict;
  }

  if (out) *out = info;
  return info.verdict;
}

const char *hg_shot_verdict_text(int verdict) {
  switch (verdict) {
    case HG_SHOT_OK:
      return "OK -- a usable frame came back";
    case HG_SHOT_ERR_ARGS:
      return "no reply to parse";
    case HG_SHOT_ERR_PARSE:
      return "the reply is not the documented shape";
    case HG_SHOT_ERR_NO_ELEMENTS:
      return "the reply's data does not match its own w/h/stride";
    case HG_SHOT_ERR_ZERO_SIZE:
      return "the reply describes a 0x0 image";
    case HG_SHOT_ERR_STRANGE:
      return "the reply carries the 'strange colors' flag";
    default:
      return "unknown verdict";
  }
}

int hg_shot_describe(const HgShotInfo *info, char *buf, int bufsize) {
  char tmp[256];
  int n = 0;
  if (!buf || bufsize <= 0) return 0;
  buf[0] = '\0';
  if (!info) {
    putStr(buf, bufsize, "(no measurement)");
    while (buf[n] != '\0') n++;
    return n;
  }

  if (info->verdict == HG_SHOT_OK) {
    snprintf(tmp, sizeof tmp, "%dx%d format=%s stride=%d data=%d bytes (declared %d) -- %s",
             info->width, info->height, info->format[0] ? info->format : "(absent)", info->stride,
             info->elements, info->declared, hg_shot_verdict_text(info->verdict));
  } else {
    snprintf(tmp, sizeof tmp, "%dx%d format=%s data=%d bytes (declared %d) missing='%s' -- %s",
             info->width, info->height, info->format[0] ? info->format : "(absent)",
             info->elements, info->declared, info->missing ? info->missing : "(nothing)",
             hg_shot_verdict_text(info->verdict));
  }
  tmp[sizeof tmp - 1] = '\0';

  putStr(buf, bufsize, tmp);
  while (buf[n] != '\0') n++;
  return n;
}
