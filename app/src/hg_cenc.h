/*
 * CENC (ISO/IEC 23001-7) sample decryption, as the media layer needs it.
 *
 * GPL-3.0-only. Ported from N3urda/hongguoTV MediaCrypto.kt (GPL-3.0), itself a
 * Kotlin adaptation of woshishiq1/drpys @22261ad (GPL-3.0). Combined work: GPL-3.0.
 *
 * Scope: the decryption half -- the sample table and the CTR walk over it. The
 * box-tree parser that BUILDS a HgCencPrepared from an MP4 is a separate step and
 * is not here yet; tests construct sample tables directly, which is also what the
 * Kotlin tests do.
 *
 * COUNTER LAYOUT, which is easy to get wrong:
 *   counter = BigInteger(sample.iv) + (relativeByteOffset / 16), 128-bit
 *   sample.iv is SIXTEEN bytes -- eight taken from the CENC auxiliary data
 *   followed by eight zero bytes -- so the auxiliary bytes form the HIGH half of
 *   the counter and the block index lands in the LOW half. Adding the index to
 *   the low half (rather than, say, treating the IV as a 64-bit value) is the
 *   whole reason hg_aes_counter_add exists.
 */

#ifndef HG_CENC_H
#define HG_CENC_H

#include "hg_aes.h"
#include "hg_crypto.h"   /* spade needs hg_sha512 / hg_unhex / hg_base64_decode;
                          * without this they are implicitly declared (TCC warns
                          * and the link still resolves, devkitA64 errors) */

#include <stddef.h>
#include <stdint.h>

#define HG_CENC_MAX_SAMPLES 64

typedef struct {
  int64_t start;             /* absolute file offset of the sample */
  int64_t end;               /* start + size */
  int64_t encrypted_offset;  /* bytes of the sample skipped before encryption */
  uint8_t iv[16];            /* 8 auxiliary bytes followed by 8 zero bytes */
} HgCencSample;

typedef struct {
  const uint8_t *header;     /* rewritten header, or NULL when unencrypted */
  size_t header_len;
  HgCencSample samples[HG_CENC_MAX_SAMPLES];
  size_t count;
} HgCencPrepared;

/* Decrypt `len` bytes of `data` that begin at absolute offset `start`, writing
 * `len` bytes to `out` (which may alias `data`).
 *
 * The header overlay is applied first, exactly as the Kotlin does: for any byte
 * of the requested range that falls inside the rewritten header, the header
 * wins. Then every sample overlapping [start, start+len) is decrypted in place
 * with the correct counter offset.
 *
 * `key == NULL` means "unencrypted": only the header overlay happens. */
void hg_cenc_decrypt(const uint8_t *data, size_t len, int64_t start,
                     const uint8_t key[HG_AES_KEY128],
                     const HgCencPrepared *prepared, uint8_t *out);

/* ---- the two "spade" crypto steps that precede playback ----------------- */

/* spade_a -> the 16-byte CENC content key. `value` is the base64 string from the
 * video model. Returns 1 on success.
 *
 * The base64 is URL-safe in the reference (`decode64` maps '-' -> '+' and
 * '_' -> '/'), and the derived bytes must spell 32 ASCII hex digits or the value
 * is rejected -- so most byte strings are not valid spade_a values. */
int hg_spade_content_key(const char *value, uint8_t out[HG_AES_KEY128]);

/* player_access_key -> the 16-byte CENC content key (build173).
 *
 * WHY THERE ARE TWO OF THESE. The play-info route -- the one that answers now -- does not return a
 * `spade_a`; it returns an opaque `player_access_key` plus a `kid`. hg_spade_content_key() above CANNOT
 * read it: measured, the live value base64-decodes to 37 bytes
 * (a3e677c63bda20946dda4df855e14ef854e21ac23ad81bc152d873926d8f7290398f239e9e), where that function
 * requires exactly 32 ASCII hex characters. So a second derivation is needed, not a parameter change.
 *
 * WHAT IT IMPLEMENTS, AND WHY THAT IS NOT A GUESS: it is the live web client's own openBox(), taken from
 * the bundle the player page loads. That bundle sets `useEME: false` for this route, so the Widevine
 * path in it is dead code and the key travels through plain JavaScript -- reproducible here. Every
 * constant below is copied from that function rather than fitted to a sample.
 *
 * PINNED TO A MEASURED VECTOR, asserted by the host tests (see hg_cenc_test.c), because a wrong key here
 * does NOT report an error -- it produces a black screen with clean logs:
 *     "o+Z3xjvaIJRt2k34VeFO+FTiGsI62BvBUthzkm2PcpA5jyOeng=="  ->  86c22eed79003130e78191b09b7588e5
 * Verified on 2026-10-16 three ways: this derivation and the bundle's own openBox() agree byte for byte
 * on that vector; the key decrypts 150 of 150 samples of a real DASH segment (invalid = 0); and the
 * decrypted bytes parse as valid H.264 (59 non-IDR NAL units + 1 IDR keyframe).
 *
 * `value` is the base64 TEXT as it arrives in play_info. Returns 1 on success, 0 -- REFUSED, never a
 * guess -- for an unrecognised method tag, a value too short to hold one, or a result that is not
 * exactly 32 hex digits. */
int hg_spade_open_box(const char *value, uint8_t out[HG_AES_KEY128]);

/* Decrypt a "spade" media URL. `value` is the base64 ciphertext from main_url,
 * `seed` the decoded key_seed. Writes a NUL-terminated UTF-8 URL to `out`.
 * Returns 1 on success. */
int hg_spade_decrypt_url(const char *value, const uint8_t *seed, size_t seed_len,
                         char *out, size_t cap);

#endif /* HG_CENC_H */
