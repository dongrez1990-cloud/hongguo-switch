/* hg_index.h -- the local search index's arithmetic: merging pages into one list.
 *
 * WHY THIS IS C AND NOT THE C++ LOOP IT REPLACED. The C++ version was a nested loop inside
 * SearchIndex::build -- correct, and completely untested, because a C++ member function that fetches from the
 * network cannot be called from the host tests. What it does is exactly the kind of thing this project keeps in
 * C:
 *
 *   - DE-DUPLICATE BY ID: the same series appears in more than one category (measured: two of the home page's
 *     "all" tier also appear in real-drama), and a search that lists the same drama twice looks like a bug;
 *   - STOP AT A CAP: the index is filled page by page, and without a cap a paging bug upstream (a page that
 *     repeats for ever) would grow it until the console runs out of memory -- a crash whose cause is nowhere
 *     near its symptom;
 *   - KEEP THE ORDER: results are shown in the order the pages arrived, so the merge must not shuffle them.
 *
 * All three can be wrong silently, so they are here with host tests. The C++ side keeps only the fetching and the
 * logging.
 *
 * ASCII-only source file.
 */
#ifndef HG_INDEX_H
#define HG_INDEX_H

#include <stddef.h>

#include "hg_api.h"

/* HOW MANY SERIES THE INDEX HOLDS. 3 categories x 6 pages x 24 = 432 at most today, so this is a ceiling that
 * exists to stop a runaway rather than to shape the index: it is what turns "upstream keeps handing us the same
 * page" from an out-of-memory crash into a log line. */
#define HG_INDEX_MAX 1024

/* What one merge did, for the caller's log line. */
typedef struct {
  int added;        /* entries appended */
  int duplicate;    /* entries skipped because their id was already in the list */
  int overflow;     /* entries dropped because the list was full */
} HgIndexMerge;

/* APPEND `in[0..n)` TO THE LIST `dst` (which holds `*count` entries), SKIPPING DUPLICATES AND STOPPING AT `cap`.
 *
 * Returns 0 on success, -1 on a NULL argument or a NULL/zero-capacity destination. `*count` and `*result` are
 * written only on success; a caller that passes a full list gets `added == 0` and `overflow == n`, not an error,
 * because "the index is full" is a normal outcome rather than a failure. */
int hg_index_append(HgSeries *dst, size_t *count, size_t cap, const HgSeries *in, size_t n,
                    HgIndexMerge *result);

/* HOW MANY of `in[0..n)` are NOT already in `dst[0..count)` by id. The same rule hg_index_append applies,
 * exposed so a caller can report "12 new of 24" without merging. A NULL id counts as a duplicate of another
 * NULL id -- upstream always sends one, and two entries with no id cannot be told apart. */
int hg_index_count_new(const HgSeries *dst, size_t count, const HgSeries *in, size_t n);

#endif /* HG_INDEX_H */
