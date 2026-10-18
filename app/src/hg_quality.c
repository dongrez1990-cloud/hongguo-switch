// hg_quality implementation. See hg_quality.h for the measured ladder this orders.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "hg_quality.h"

#include <stdio.h>
#include <string.h>

// Bounded copy that always terminates. snprintf's truncation behaviour is not portable in
// this project's toolchain -- MEASURED: TinyCC returns -1 and leaves the buffer unterminated
// for an exactly-sized buffer -- so it is used only for building a private string, never to
// write directly into a caller's buffer.
static void putStr(char* dst, int cap, const char* src) {
  int i = 0;
  if (!dst || cap <= 0) return;
  if (!src) src = "";
  while (src[i] != '\0' && i < cap - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

// Stable insertion sort, descending by (height, bitrate). Insertion sort rather than qsort
// because the list is at most 16 long and a STABLE sort is a requirement: two rows with the
// same height and bitrate must keep the order upstream sent them in, or the menu would
// shuffle between runs for no reason the user could see.
static void sortDescending(HgQualityTier* a, int n) {
  int i;
  for (i = 1; i < n; i++) {
    HgQualityTier key = a[i];
    int j = i - 1;
    while (j >= 0) {
      int worse = (a[j].height < key.height) ||
                  (a[j].height == key.height && a[j].bitrate < key.bitrate);
      if (!worse) break;
      a[j + 1] = a[j];
      j--;
    }
    a[j + 1] = key;
  }
}

int hg_quality_build(const int* quality_types, const char* const* definitions,
                     const int* widths, const int* heights, const int* bitrates,
                     const long long* sizes, const int* decodable, const char* const* codecs, int n,
                     HgQualityTier* out, int cap) {
  int i;
  int count = 0;
  int offer;

  if (!out || cap <= 0) return 0;
  if (n <= 0 || !widths || !heights) return 0;

  /* SORT FIRST, TRUNCATE AFTER.
   *
   * The first version truncated the offer to `cap` and then sorted, which means a cap
   * smaller than the offer kept the FIRST `cap` ENTRIES AS UPSTREAM SENT THEM -- and upstream
   * sends worst-first (40 = 360p is entry 0). So a cap of 3 offered 360p, 480p and 540p and
   * called 360p the best tier. The host suite caught it ("keeps the BEST three"); nothing on
   * screen would have.
   *
   * `work` is therefore sized for the WHOLE offer, HG_QUALITY_MAX being the ceiling hg_api.h
   * already imposes, and only the best `cap` survive into the caller's array. */
  {
    HgQualityTier work[HG_QUALITY_MAX];
    offer = n;
    if (offer > HG_QUALITY_MAX) offer = HG_QUALITY_MAX;

    for (i = 0; i < offer; i++) {
      HgQualityTier* t = &work[i];
      memset(t, 0, sizeof *t);
      t->quality_type = quality_types ? quality_types[i] : 0;
      putStr(t->definition, (int)sizeof t->definition, definitions ? definitions[i] : "");
      putStr(t->codec, (int)sizeof t->codec, codecs && codecs[i] ? codecs[i] : "");
      t->width = widths[i];
      t->height = heights[i];
      t->bitrate = bitrates ? bitrates[i] : 0;
      t->size_bytes = sizes ? sizes[i] : 0;
      /* The source index is the position in the OFFERED list, recorded BEFORE the sort so it
       * still refers to the right entry afterwards. Getting this wrong would make "switch to
       * 720p" build a plan from 360p -- a wrong picture with no error, which is the exact
       * class of failure this file exists to prevent. */
      t->source_index = i;
      t->decodable = decodable ? decodable[i] : 1;
    }

    sortDescending(work, offer);

    count = offer < cap ? offer : cap;
    for (i = 0; i < count; i++) out[i] = work[i];
  }

  return count;
}

int hg_quality_index_of(const HgQualityTier* tiers, int count, int qualityType) {
  int i;
  if (!tiers || count <= 0) return -1;
  for (i = 0; i < count; i++) {
    if (tiers[i].quality_type == qualityType) return i;
  }
  return -1;
}

int hg_quality_label(const HgQualityTier* tier, char* buf, int bufsize) {
  char tmp[48];
  int len = 0;

  if (!buf || bufsize <= 0) return 0;

  if (!tier) {
    putStr(buf, bufsize, "unknown");
    while (buf[len] != '\0') len++;
    return len;
  }

  if (tier->definition[0] != '\0') {
    // Upstream's own label, and the one the user is most likely to recognise.
    putStr(buf, bufsize, tier->definition);
  } else if (tier->height > 0) {
    snprintf(tmp, sizeof tmp, "%dp", tier->height);
    tmp[sizeof tmp - 1] = '\0';
    putStr(buf, bufsize, tmp);
  } else if (tier->quality_type != 0) {
    snprintf(tmp, sizeof tmp, "quality %d", tier->quality_type);
    tmp[sizeof tmp - 1] = '\0';
    putStr(buf, bufsize, tmp);
  } else {
    putStr(buf, bufsize, "unknown");
  }

  /* WHERE THE LABEL ENDS, COMPUTED BEFORE ANYTHING IS APPENDED.
   *
   * The first version of this appended at `buf + len` while `len` was still 0, so the codec OVERWROTE the
   * resolution instead of following it and every tier would have read "  ·  HEVC". Four label checks caught it
   * immediately -- which is the reason this arithmetic lives in C with tests. */
  len = 0;
  while (buf[len] != '\0') len++;

  /* THE CODEC, APPENDED. It is here rather than in a row of its own because it is a property OF the tier: the
   * user picking between 1080p and 720p is also picking between codecs, and on this console that matters -- the
   * handover's stutter report is about the software decode path, where HEVC costs much more than H.264. */
  if (tier->codec[0] != '\0') {
    const char* name = tier->codec;
    if (strcmp(tier->codec, "h264") == 0) name = "H.264";
    else if (strcmp(tier->codec, "bytevc1") == 0) name = "HEVC";
    /* bytevc2 keeps upstream's own spelling: its real identity is unmeasured. */
    {
      char tail[32];
      snprintf(tail, sizeof tail, "  \xC2\xB7  %s", name);   /* " · " -- U+00B7 */
      tail[sizeof tail - 1] = '\0';
      putStr(buf + len, bufsize - len, tail);
    }
  }

  len = 0;
  while (buf[len] != '\0') len++;
  return len;
}

int hg_quality_describe(const HgQualityTier* tiers, int count, char* buf, int bufsize) {
  char one[48];
  char line[320];
  int i;
  int n = 0;

  if (!buf || bufsize <= 0) return 0;
  buf[0] = '\0';
  if (!tiers || count <= 0) {
    putStr(buf, bufsize, "(no tiers)");
    while (buf[n] != '\0') n++;
    return n;
  }

  line[0] = '\0';
  for (i = 0; i < count; i++) {
    char piece[80];
    hg_quality_label(&tiers[i], one, (int)sizeof one);
    // A tier the player cannot decode is marked here too, so the log and the menu agree
    // about which rows are offers and which are information.
    snprintf(piece, sizeof piece, "%s%s%s", i ? " > " : "", one,
             tiers[i].decodable ? "" : " (undecodable)");
    piece[sizeof piece - 1] = '\0';
    if (strlen(line) + strlen(piece) + 1 >= sizeof line) break;
    strcat(line, piece);
  }

  putStr(buf, bufsize, line);
  while (buf[n] != '\0') n++;
  return n;
}
