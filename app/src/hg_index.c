/* hg_index.c -- see hg_index.h for why the index's merge policy lives here.
 *
 * ASCII-only source file. */

#include "hg_index.h"

#include <string.h>

/* TWO IDS ARE THE SAME WHEN THE STRINGS MATCH. An empty id is treated as a value like any other: upstream sends
 * one for every card (`series_id`), so an empty one means a malformed entry, and merging two malformed entries
 * into one is the lesser evil -- the alternative is a list that grows by duplicates of the same broken record. */
static int same_id(const HgSeries *a, const HgSeries *b) {
  return strcmp(a->id, b->id) == 0;
}

static int is_duplicate(const HgSeries *dst, size_t count, const HgSeries *one) {
  size_t k;
  for (k = 0; k < count; k++) {
    if (same_id(&dst[k], one)) return 1;
  }
  return 0;
}

int hg_index_count_new(const HgSeries *dst, size_t count, const HgSeries *in, size_t n) {
  size_t i;
  int fresh = 0;

  if (dst == NULL || in == NULL) return 0;
  /* WITHIN `in` ITSELF TOO: one page can repeat an entry, and the second copy is not new. Counting it would make
   * the log say "24 new" for a page that added 23. */
  for (i = 0; i < n; i++) {
    size_t k;
    int seen = is_duplicate(dst, count, &in[i]);
    for (k = 0; !seen && k < i; k++) {
      if (same_id(&in[k], &in[i])) seen = 1;
    }
    if (!seen) fresh++;
  }
  return fresh;
}

int hg_index_append(HgSeries *dst, size_t *count, size_t cap, const HgSeries *in, size_t n,
                    HgIndexMerge *result) {
  size_t i;
  size_t at;

  if (result != NULL) {
    result->added = 0;
    result->duplicate = 0;
    result->overflow = 0;
  }
  if (dst == NULL || count == NULL || in == NULL || cap == 0) return -1;
  if (*count > cap) return -1;   /* a caller that lost track of its own count is a bug, not an overflow */

  at = *count;
  for (i = 0; i < n; i++) {
    /* THE CAP IS CHECKED BEFORE THE DUPLICATE TEST, and the order matters for the report: an entry that is both
     * a duplicate and past the end is a duplicate, because that is what the user would see (it is not in the
     * list twice). */
    if (is_duplicate(dst, at, &in[i])) {
      if (result != NULL) result->duplicate++;
      continue;
    }
    if (at >= cap) {
      if (result != NULL) result->overflow++;
      continue;
    }
    dst[at] = in[i];
    at++;
    if (result != NULL) result->added++;
  }

  *count = at;
  return 0;
}
