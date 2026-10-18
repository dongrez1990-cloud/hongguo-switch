/*
 * 红果 request signing: the header-producing layer built on hg_crypto.
 *
 * GPL-3.0-only. Ported from N3urda/hongguoTV kotlin-tv/core Signer.kt
 * (GPL-3.0), cross-checked against the reference it derives from,
 * server/vendor/hongguo.cjs, following woshishiq1/drpys @22261ad (GPL-3.0).
 * The combined work stays GPL-3.0.
 *
 * WHAT IS HERE
 *   getIv / sumMd5      the two mixing helpers
 *   branchOf            which hash branch a request falls into
 *   hashF13             the "x-ss-req-ticket" digest, branches 0 and 2
 *   xGorgon             the "x-gorgon" header (uses the abridged RC4)
 *   helios              the "x-helios" header
 *
 * medusa is in hg_medusa.c (it would not fit here).
 *
 * BRANCH COVERAGE
 *   Branches 0 and 2 are implemented. Branch 1 is refused.
 *
 *   That split is not arbitrary -- it is what the reference does. The video
 *   model request loops until the branch is NOT 1:
 *       if (branchOf(url, body, khronos) !== 1) break;    hongguo.cjs:428
 *   and the comment there says the server does not accept branch 1's variant.
 *   So in the field a request lands on 0 or 2. An implementation with only
 *   branch 0 fails roughly five attempts in sixteen -- and the search endpoint
 *   has no retry loop at all, so those are simply broken requests.
 *
 *   Branch 1 is refused rather than approximated: its output is 36 bytes rather
 *   than 20, so returning it through the 20-byte buffer would overrun.
 */

#ifndef HG_SIGN_H
#define HG_SIGN_H

#include "hg_crypto.h"

#include <stddef.h>
#include <stdint.h>

/* Returns one full 32-bit draw, matching Math.floor(Math.random() * 2^32) in
 * the reference. The test harness supplies the same seeded LCG the oracle used,
 * so signatures are reproducible. */
typedef uint32_t (*hg_rand32_fn)(void *user);

/* Lengths in bytes. */
#define HG_HASH_F13_LEN   20   /* hex: 40 chars */
#define HG_GORGON_LEN     26   /* hex: 52 chars */
#define HG_HELIOS_MAX     128  /* base64; 48 chars for the current inputs */

/* The two mixing primitives. hg_sum_md5 reads only data[0..11]. */
uint32_t hg_get_iv(uint32_t iv, const uint8_t *data, size_t len);
uint32_t hg_sum_md5(const uint8_t *data);

/* Which of the three hash branches this request lands in (0, 1 or 2).
 * `body` may be NULL for a GET. */
uint32_t hg_branch_of(const char *query, const uint8_t *body, size_t body_len,
                      uint32_t khronos);

/* hashF13 -> raw 20 bytes. medusa embeds these bytes, not their hex form.
 * Returns 1 on success, 0 if the request landed outside branch 0. */
int hg_hash_f13_bytes(const uint8_t query_sm3[32], const uint8_t body_md5[16],
                      uint32_t khronos, uint8_t out[HG_HASH_F13_LEN]);

/* Same, as lowercase hex (write 41 bytes). */
int hg_hash_f13(const uint8_t query_sm3[32], const uint8_t body_md5[16],
                uint32_t khronos, char out_hex[HG_HASH_F13_LEN * 2 + 1]);

/* xGorgon -> x-gorgon value, lowercase hex (write HG_GORGON_LEN*2+1 bytes).
 * `random` is the request nonce; the reference passes a constant 42131 when the
 * value only needs to be well-formed. */
void hg_xgorgon(const char *query, const uint8_t *body, size_t body_len,
                uint32_t timestamp, uint32_t random,
                char out_hex[HG_GORGON_LEN * 2 + 1]);

/* helios -> x-helios value, base64 (NUL-terminated). Returns 1 on success. */
int hg_helios(uint32_t khronos, hg_rand32_fn rnd, void *user,
              char *out_b64, size_t cap);

/* medusa -> x-medusa value, base64. Returns 1 on success.
 *
 * DRAW ORDER IS PART OF THE CONTRACT. buildMedusa consumes FIVE draws, in this
 * order: messageRand, envLaunch, envPid, random, xmRand. In the oracle's field
 * order helios is evaluated first, so within a row medusa's draws are #2..#6.
 * Reseeding or reordering changes the output.
 *
 * The request ticket must already be a branch-0 one (see hg_hash_f13_bytes);
 * this returns 0 rather than signing with a different branch. */
#define HG_MEDUSA_MAX 1400
int hg_build_medusa(const char *url, const uint8_t *body, size_t body_len,
                    uint32_t khronos, hg_rand32_fn rnd, void *user,
                    char *out_b64, size_t cap);

#endif /* HG_SIGN_H */
