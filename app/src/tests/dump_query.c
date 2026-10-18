/*
 * Dump the C implementation's search query string so it can be diffed against
 * the reference's. Temporary diagnostic.
 */

#include <stdio.h>
#include <string.h>

#include "hg_request.h"
#include "hg_sign.h"

int main(int argc, char **argv) {
  static HgRequest req;
  /* Fixed inputs; TinyCC has no strtoull and the comparison only needs one pair
   * of values, which the Node side is given as arguments. */
  uint32_t khronos = 1791466175u;
  uint64_t ticket = 1791466175000ull;
  int rc;

  (void)argc; (void)argv;

  rc = hg_request_search(&req, "\xe7\x9f\xad\xe5\x89\xa7", khronos, ticket, 42131u);
  if (rc != 0) { fprintf(stderr, "hg_request_search rc=%d\n", rc); return 1; }

  fputs(req.query, stdout);
  fprintf(stderr, "\nc query length = %u\n", (unsigned)strlen(req.query));
  return 0;
}
