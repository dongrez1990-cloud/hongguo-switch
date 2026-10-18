/*
 * ISO-BMFF header parsing for CENC. See hg_mp4.h for provenance and the
 * fail-closed contract. Line references are to MediaCrypto.kt.
 */

#include "hg_mp4.h"

#include <stdlib.h>
#include <string.h>

typedef struct { int64_t start, end, body; char type[5]; } HgBox;

static uint32_t u32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static int32_t i32(const uint8_t *p) { return (int32_t)u32(p); }
static int64_t i64(const uint8_t *p) {
  uint64_t v = 0;
  int i;
  for (i = 0; i < 8; i++) v = (v << 8) | (uint64_t)p[i];
  return (int64_t)v;
}

/* box() at MediaCrypto.kt:28 -- note the three size forms and the two bounds
 * checks; a zero size means "to the end of the parent". */
static int box_at(const uint8_t *d, size_t len, int64_t at, int64_t limit, HgBox *out) {
  int64_t size, header = 8;
  if (at < 0 || at + 8 > limit || at + 8 > (int64_t)len) return 0;
  size = (int64_t)u32(d + at);
  if (size == 1) {
    if (at + 16 > limit || at + 16 > (int64_t)len) return 0;
    size = i64(d + at + 8);
    header = 16;
  }
  if (size == 0) size = limit - at;
  if (size < header || size > limit - at) return 0;
  out->start = at;
  out->end = at + size;
  out->body = at + header;
  memcpy(out->type, d + at + 4, 4);
  out->type[4] = '\0';
  return 1;
}

/* find() at MediaCrypto.kt:36 -- a linear scan, cheap because the reference
 * checks the type byte before validating the box. */
static int find_box(const uint8_t *d, size_t len, const char *type,
                    int64_t start, int64_t limit, HgBox *out) {
  int64_t at;
  for (at = start; at <= limit - 8 && at + 8 <= (int64_t)len; at++) {
    HgBox b;
    if (d[at + 4] != (uint8_t)type[0]) continue;
    if (!box_at(d, len, at, limit, &b)) continue;
    if (memcmp(b.type, type, 4) == 0) { *out = b; return 1; }
  }
  return 0;
}

/* count() at MediaCrypto.kt:63 -- every index count in this format is a u32 with
 * a sanity ceiling. */
static int read_count(const uint8_t *d, size_t len, int64_t at, int64_t limit, uint32_t *out) {
  if (at + 4 > limit || at + 4 > (int64_t)len) return 0;
  *out = u32(d + at);
  if (*out > 200000u) return 0;
  return 1;
}

int hg_mp4_header_end(const uint8_t *first, size_t first_len, int64_t total) {
  int64_t ftyp, size, end;
  if (first_len < 16) return HG_MP4_ERR_CONTAINER;
  if (memcmp(first + 4, "ftyp", 4) != 0) return HG_MP4_ERR_CONTAINER;
  ftyp = (int64_t)u32(first);
  if (ftyp < 8 || ftyp + 8 > (int64_t)first_len) return HG_MP4_ERR_CONTAINER;
  if (memcmp(first + ftyp + 4, "moov", 4) != 0) return HG_MP4_ERR_CONTAINER;
  size = (int64_t)u32(first + ftyp);
  end = ftyp + size;
  /* The reference caps moov at 8 MiB; a larger one means we are reading garbage. */
  if (size < 8 || end > total || end > 8 * 1024 * 1024) return HG_MP4_ERR_INDEX;
  return (int)end;
}

int hg_mp4_prepare(const uint8_t *data, size_t len, const uint8_t key[16],
                   uint8_t *header_out, HgCencPrepared *prepared) {
  HgBox moov;
  int64_t cursor;
  size_t i;

  if (header_out == NULL || prepared == NULL) return HG_MP4_ERR_LAYOUT;
  memcpy(header_out, data, len);
  prepared->header = header_out;
  prepared->header_len = len;
  prepared->count = 0;

  if (key == NULL) return 0;                 /* unencrypted: header only */

  if (!box_at(data, len, (int64_t)u32(data), (int64_t)len, &moov)) return HG_MP4_ERR_LAYOUT;
  if (memcmp(moov.type, "moov", 4) != 0) return HG_MP4_ERR_LAYOUT;

  cursor = moov.body;
  while (cursor < moov.end) {
    HgBox track, stbl, saiz, saio, stsz, stsc, chunks;
    uint32_t n, default_size, chunk_count, entry_count, aux_count;
    int64_t size_base, offset_base, aux, aux_end, aux_sum = 0;
    int offset_width;
    uint32_t entry = 0, sample_index = 0;
    uint32_t *sizes = NULL, *aux_sizes = NULL;
    int64_t *offsets = NULL;
    uint32_t (*entries)[2] = NULL;
    int width;

    if (!box_at(data, len, cursor, moov.end, &track)) return HG_MP4_ERR_LAYOUT;
    cursor = track.end;
    if (memcmp(track.type, "trak", 4) != 0) continue;

    if (!find_box(data, len, "stbl", track.body, track.end, &stbl)) continue;
    if (!find_box(data, len, "saiz", stbl.body, stbl.end, &saiz)) continue;
    if (!find_box(data, len, "saio", stbl.body, stbl.end, &saio)) return HG_MP4_ERR_LAYOUT;
    if (!find_box(data, len, "stsz", stbl.body, stbl.end, &stsz)) return HG_MP4_ERR_LAYOUT;
    if (!find_box(data, len, "stsc", stbl.body, stbl.end, &stsc)) return HG_MP4_ERR_LAYOUT;
    if (!find_box(data, len, "stco", stbl.body, stbl.end, &chunks)) {
      if (!find_box(data, len, "co64", stbl.body, stbl.end, &chunks)) return HG_MP4_ERR_LAYOUT;
    }

    if (!read_count(data, len, stsz.body + 8, stsz.end, &n)) return HG_MP4_ERR_TRUNCATED;
    if (n > HG_CENC_MAX_SAMPLES) return HG_MP4_ERR_TOO_MANY;
    default_size = u32(data + stsz.body + 4);
    if (default_size == 0 && stsz.body + 12 + (int64_t)n * 4 > stsz.end) return HG_MP4_ERR_TRUNCATED;

    sizes = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    aux_sizes = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (sizes == NULL || aux_sizes == NULL) {
      free(sizes); free(aux_sizes);
      return HG_MP4_ERR_LAYOUT;
    }
    for (i = 0; i < n; i++)
      sizes[i] = (default_size > 0) ? default_size : u32(data + stsz.body + 12 + (int64_t)i * 4);

    if (!read_count(data, len, chunks.body + 4, chunks.end, &chunk_count)) {
      free(sizes); free(aux_sizes); return HG_MP4_ERR_TRUNCATED;
    }
    width = (memcmp(chunks.type, "co64", 4) == 0) ? 8 : 4;
    if (chunks.body + 8 + (int64_t)chunk_count * width > chunks.end) {
      free(sizes); free(aux_sizes); return HG_MP4_ERR_TRUNCATED;
    }
    offsets = (int64_t *)malloc(((size_t)chunk_count + 1) * sizeof(int64_t));
    if (offsets == NULL) { free(sizes); free(aux_sizes); return HG_MP4_ERR_LAYOUT; }
    for (i = 0; i < chunk_count; i++) {
      offsets[i] = (width == 8) ? i64(data + chunks.body + 8 + (int64_t)i * width)
                                : (int64_t)u32(data + chunks.body + 8 + (int64_t)i * width);
    }

    if (!read_count(data, len, stsc.body + 4, stsc.end, &entry_count)) {
      free(sizes); free(aux_sizes); free(offsets); return HG_MP4_ERR_TRUNCATED;
    }
    if (stsc.body + 8 + (int64_t)entry_count * 12 > stsc.end || entry_count == 0) {
      free(sizes); free(aux_sizes); free(offsets); return HG_MP4_ERR_TRUNCATED;
    }
    entries = (uint32_t (*)[2])malloc(((size_t)entry_count + 1) * sizeof(*entries));
    if (entries == NULL) { free(sizes); free(aux_sizes); free(offsets); return HG_MP4_ERR_LAYOUT; }
    for (i = 0; i < entry_count; i++) {
      entries[i][0] = u32(data + stsc.body + 8 + (int64_t)i * 12);
      entries[i][1] = u32(data + stsc.body + 12 + (int64_t)i * 12);
    }
    entries[entry_count][0] = 0xffffffffu;   /* sentinel: never matches a chunk */
    entries[entry_count][1] = entries[entry_count - 1][1];

    /* ISO-BMFF auxiliary-info flags may insert an eight-byte type/parameter pair. */
    size_base = saiz.body + 4 + ((i32(data + saiz.body) & 1) ? 8 : 0);
    if (size_base + 5 > saiz.end) {
      free(sizes); free(aux_sizes); free(offsets); free(entries);
      return HG_MP4_ERR_TRUNCATED;
    }
    {
      uint32_t aux_default = data[size_base];
      if (!read_count(data, len, size_base + 1, saiz.end, &aux_count)) {
        free(sizes); free(aux_sizes); free(offsets); free(entries);
        return HG_MP4_ERR_TRUNCATED;
      }
      if (aux_count != n || (aux_default == 0 && size_base + 5 + (int64_t)n > saiz.end)) {
        free(sizes); free(aux_sizes); free(offsets); free(entries);
        return HG_MP4_ERR_TRUNCATED;
      }
      for (i = 0; i < n; i++)
        aux_sizes[i] = (aux_default > 0) ? aux_default : data[size_base + 5 + (int64_t)i];
    }

    offset_base = saio.body + 4 + ((i32(data + saio.body) & 1) ? 8 : 0);
    offset_width = (data[saio.body] == 1) ? 8 : 4;
    {
      uint32_t one = 0;
      if (!read_count(data, len, offset_base, saio.end, &one) || one != 1 ||
          offset_base + 4 + offset_width > saio.end) {
        free(sizes); free(aux_sizes); free(offsets); free(entries);
        return HG_MP4_ERR_UNSUPPORTED;
      }
    }
    aux = (offset_width == 8) ? i64(data + offset_base + 4) : (int64_t)u32(data + offset_base + 4);
    for (i = 0; i < n; i++) aux_sum += (int64_t)aux_sizes[i];
    aux_end = aux + aux_sum;
    if (aux < 0 || aux_end > (int64_t)len) {
      free(sizes); free(aux_sizes); free(offsets); free(entries);
      return HG_MP4_ERR_TRUNCATED;
    }

    for (i = 0; i < chunk_count; i++) {
      uint32_t count, k;
      int64_t position;
      while (entry + 1 < entry_count && (int64_t)entries[entry + 1][0] <= (int64_t)i + 1) entry++;
      count = entries[entry][1];
      if ((int64_t)count > (int64_t)n - (int64_t)sample_index ||
          (int64_t)entries[entry][0] > (int64_t)i + 1) {
        free(sizes); free(aux_sizes); free(offsets); free(entries);
        return HG_MP4_ERR_LAYOUT;
      }
      position = offsets[i];
      for (k = 0; k < count; k++) {
        int64_t length = (int64_t)sizes[sample_index];
        int64_t aux_size = (int64_t)aux_sizes[sample_index];
        HgCencSample *s;
        if (sample_index >= n) { free(sizes); free(aux_sizes); free(offsets); free(entries); return HG_MP4_ERR_LAYOUT; }
        if (position < 0 || length <= 0 || position > INT64_MAX - length || aux_size < 8) {
          free(sizes); free(aux_sizes); free(offsets); free(entries);
          return HG_MP4_ERR_LAYOUT;
        }
        if (prepared->count >= HG_CENC_MAX_SAMPLES) {
          free(sizes); free(aux_sizes); free(offsets); free(entries);
          return HG_MP4_ERR_TOO_MANY;
        }
        s = &prepared->samples[prepared->count];
        memset(s, 0, sizeof *s);
        memcpy(s->iv, data + aux, 8);
        if (aux_size == 8) {
          s->start = position;
          s->end = position + length;
          s->encrypted_offset = 0;
          prepared->count++;
        } else {
          int64_t offset = position, encrypted = 0;
          uint32_t subs, sub;
          if (aux_size < 10) { free(sizes); free(aux_sizes); free(offsets); free(entries); return HG_MP4_ERR_UNSUPPORTED; }
          subs = ((uint32_t)data[aux + 8] << 8) | (uint32_t)data[aux + 9];
          if (aux_size != 10 + (int64_t)subs * 6) {
            free(sizes); free(aux_sizes); free(offsets); free(entries);
            return HG_MP4_ERR_TRUNCATED;
          }
          for (sub = 0; sub < subs; sub++) {
            int64_t at = aux + 10 + (int64_t)sub * 6;
            int64_t clear = ((int64_t)data[at] << 8) | (int64_t)data[at + 1];
            int64_t subsize = (int64_t)u32(data + at + 2);
            offset += clear;
            if (offset + subsize > position + length) {
              free(sizes); free(aux_sizes); free(offsets); free(entries);
              return HG_MP4_ERR_TRUNCATED;
            }
            if (subsize > 0) {
              if (prepared->count >= HG_CENC_MAX_SAMPLES) {
                free(sizes); free(aux_sizes); free(offsets); free(entries);
                return HG_MP4_ERR_TOO_MANY;
              }
              s = &prepared->samples[prepared->count];
              memset(s, 0, sizeof *s);
              memcpy(s->iv, data + aux, 8);
              s->start = offset;
              s->end = offset + subsize;
              s->encrypted_offset = encrypted;
              prepared->count++;
            }
            encrypted += subsize;
            offset += subsize;
          }
          if (offset != position + length) {
            free(sizes); free(aux_sizes); free(offsets); free(entries);
            return HG_MP4_ERR_TRUNCATED;
          }
        }
        position += length;
        aux += aux_size;
        sample_index++;
      }
    }

    free(sizes); free(aux_sizes); free(offsets); free(entries);
    if (sample_index != n) return HG_MP4_ERR_LAYOUT;

    /* Rewrite the header: keep the ORIGINAL codec fourcc from frma rather than
     * hardcoding one, blank the sinf box, and rename it "free" so the player
     * skips it. MediaCrypto.kt:114-127. */
    {
      int64_t at = stbl.body;
      while (at + 8 <= stbl.end) {
        HgBox sinf, frma;
        uint8_t original[4];
        int64_t entry_at;
        if (!find_box(data, len, "sinf", at, stbl.end, &sinf)) break;
        if (!find_box(data, len, "frma", sinf.body, sinf.end, &frma)) return HG_MP4_ERR_LAYOUT;
        if (frma.body + 4 > frma.end) return HG_MP4_ERR_LAYOUT;
        memcpy(original, data + frma.body, 4);
        entry_at = sinf.start - 1;
        while (entry_at >= stbl.body) {
          if (memcmp(data + entry_at, "encv", 4) == 0 || memcmp(data + entry_at, "enca", 4) == 0) {
            memcpy(header_out + entry_at, original, 4);
            break;
          }
          entry_at--;
        }
        memcpy(header_out + sinf.start + 4, "free", 4);
        memset(header_out + sinf.body, 0, (size_t)(sinf.end - sinf.body));
        at = sinf.end;
      }
    }
  }

  if (prepared->count == 0) return HG_MP4_ERR_NO_SAMPLES;

  /* insertion sort by start -- the reference sorts too, then rejects overlaps so
   * that hg_cenc_decrypt's binary search is valid. */
  for (i = 1; i < prepared->count; i++) {
    HgCencSample tmp = prepared->samples[i];
    size_t j = i;
    while (j > 0 && prepared->samples[j - 1].start > tmp.start) {
      prepared->samples[j] = prepared->samples[j - 1];
      j--;
    }
    prepared->samples[j] = tmp;
  }
  for (i = 1; i < prepared->count; i++) {
    if (prepared->samples[i - 1].end > prepared->samples[i].start) return HG_MP4_ERR_LAYOUT;
  }
  return 0;
}
