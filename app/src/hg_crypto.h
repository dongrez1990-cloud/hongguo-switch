/* hg_crypto.h -- self-contained crypto primitives for the hongguo signer.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported to C from N3urda/hongguoTV (GPL-3.0-only), whose kotlin-tv/core module
 * in turn derives from woshishiq1/drpys spider/js "hongguo [short]" @22261ad
 * (GPL-3.0).  The combined work stays GPL-3.0 compatible.
 *
 * C99, no external libraries, no dynamic allocation, no stdio.
 * Every integer type here is fixed width so the same code is bit-exact on
 * i386 (ILP32, sizeof(long)==4) and on aarch64 (LP64, sizeof(long)==8).
 */
#ifndef HG_CRYPTO_H
#define HG_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* MD5 (RFC 1321)                                                      */
/* ------------------------------------------------------------------ */
#define HG_MD5_DIGEST_LEN 16
/* Streaming context; the whole struct is caller-owned (no allocation). */
typedef struct {
    uint32_t state[4];
    uint64_t bitlen;      /* total message length in bits */
    uint8_t  buffer[64];
    size_t   buffer_len;
} hg_md5_ctx;

void hg_md5_init(hg_md5_ctx *ctx);
void hg_md5_update(hg_md5_ctx *ctx, const void *data, size_t len);
void hg_md5_final(hg_md5_ctx *ctx, uint8_t out[HG_MD5_DIGEST_LEN]);
void hg_md5(const void *data, size_t len, uint8_t out[HG_MD5_DIGEST_LEN]);

/* ------------------------------------------------------------------ */
/* SHA-512 (FIPS 180-4)                                                */
/* ------------------------------------------------------------------ */
#define HG_SHA512_DIGEST_LEN 64
#define HG_SHA512_BLOCK_LEN  128
typedef struct {
    uint64_t state[8];
    uint64_t bitlen;                        /* low 64 bits of the bit count */
    uint8_t  buffer[HG_SHA512_BLOCK_LEN];
    size_t   buffer_len;
} hg_sha512_ctx;

void hg_sha512_init(hg_sha512_ctx *ctx);
void hg_sha512_update(hg_sha512_ctx *ctx, const void *data, size_t len);
void hg_sha512_final(hg_sha512_ctx *ctx, uint8_t out[HG_SHA512_DIGEST_LEN]);
void hg_sha512(const void *data, size_t len, uint8_t out[HG_SHA512_DIGEST_LEN]);

/* ------------------------------------------------------------------ */
/* SM3 (GB/T 32905-2016)                                               */
/* ------------------------------------------------------------------ */
#define HG_SM3_DIGEST_LEN 32
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[64];
    size_t   buffer_len;
} hg_sm3_ctx;

void hg_sm3_init(hg_sm3_ctx *ctx);
void hg_sm3_update(hg_sm3_ctx *ctx, const void *data, size_t len);
void hg_sm3_final(hg_sm3_ctx *ctx, uint8_t out[HG_SM3_DIGEST_LEN]);
void hg_sm3(const void *data, size_t len, uint8_t out[HG_SM3_DIGEST_LEN]);

/* ------------------------------------------------------------------ */
/* "RC4" -- the vendor's abridged variant, NOT RFC 6229 RC4.             */
/*                                                                      */
/* Verified against the reference implementation in                     */
/* server/vendor/hongguo.cjs:360-364 (rc4Gorgon), which differs from    */
/* textbook RC4 in three ways:                                          */
/*                                                                      */
/*   KSA  : j = (j + s[i] + key[i % keylen]) & 255; s[i] = s[j];        */
/*          the second half of the swap (s[j] = old s[i]) is MISSING,   */
/*          so the state is not a permutation.                          */
/*   PRGA : i starts at 0 and is PRE-incremented, so the first index    */
/*          used is 1, not 0.                                           */
/*   out  : the keystream byte is s[(y + y) & 255], not s[y]; and the   */
/*          write-back is only s[i] = y, again without completing a     */
/*          swap.                                                       */
/*                                                                      */
/* A textbook RC4 produces a completely different header and every      */
/* signed request fails, so these three details are load-bearing.       */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t s[256];
    uint8_t i;
    uint8_t j;
} hg_rc4_ctx;

void hg_rc4_init(hg_rc4_ctx *ctx, const uint8_t *key, size_t key_len);
uint8_t hg_rc4_next(hg_rc4_ctx *ctx, uint8_t in);
void hg_rc4_apply(hg_rc4_ctx *ctx, const uint8_t *in, uint8_t *out, size_t len);

/* ------------------------------------------------------------------ */
/* "AesV3" -- NOT standard AES.  Protocol-specific transform documented  */
/* in AesV3.kt: a 256-byte S-box selected by `timestamp & 3`, a 16-byte  */
/* permutation order table, 2 rounds with a single MixColumns pass.      */
/*                                                                       */
/* Verified against server/vendor/hongguo.cjs:379-388.  Two details are  */
/* easy to get wrong and both were checked against the reference:        */
/*                                                                       */
/*   * The reference declares BOTH `con` and `con2`.  Every actual use -- */
/*     addCon(), sub() and shiftCon() -- reads `con2`; `con` is dead code */
/*     and never referenced.  The Kotlin port kept only one table, equal  */
/*     to con2, which is therefore correct.  Do not "fix" it toward       */
/*     `con`, whose entry [1] differs ([1,3,0,2] vs [2,0,3,1]).           */
/*   * A round key is FOUR byte-values, not sixteen: `keys` is 12 x 4     */
/*     (expanded[] holds byte values 0..255).  An earlier draft of this   */
/*     header said 192 and would have read out of bounds.                 */
/* ------------------------------------------------------------------ */

#define HG_AESV3_BLOCK_LEN 16
#define HG_AESV3_ROUND_KEYS 12   /* 12 round keys ... */
#define HG_AESV3_KEY_LEN 48      /* ... of 4 byte-values each */

/* The S-box quarter and the permutation tables are selected by
 * (timestamp & 3), so a block transform needs the context, not just the
 * round keys. */
typedef struct {
    uint32_t word;                   /* timestamp & 3 */
    uint8_t  keys[HG_AESV3_KEY_LEN]; /* keys[k*4 + n], byte values */
} hg_aesv3_ctx;

/* Build the context.  `key16` is the 16-byte key. */
void hg_aesv3_init(hg_aesv3_ctx *ctx, const uint8_t key16[HG_AESV3_BLOCK_LEN],
                   uint32_t timestamp);

/* Transform one 16-byte block.  `in` and `out` may not overlap. */
void hg_aesv3_block(const hg_aesv3_ctx *ctx,
                    const uint8_t in[HG_AESV3_BLOCK_LEN],
                    uint8_t out[HG_AESV3_BLOCK_LEN]);

/* Full "encrypt": writes 1 + source_len bytes to `out` -- the last key byte
 * followed by a copy of `source` with 31 bytes patched.  source_len must be
 * >= 248.  `out` must have room for source_len + 1 bytes. */
void hg_aesv3_encrypt(const hg_aesv3_ctx *ctx,
                      const uint8_t iv[HG_AESV3_BLOCK_LEN],
                      const uint8_t *source, size_t source_len,
                      uint8_t *out);

/* ------------------------------------------------------------------ */
/* Small shared helpers                                                */
/* ------------------------------------------------------------------ */

/* Rotations. These MUST be declared here and not only defined in the .c:
 * a caller without a prototype gets an implicit `int` return from TinyCC, which
 * silently truncates hg_ror64()'s 64-bit result to 32 bits. That produced a
 * signer where hashF13 (32-bit rotations only) happened to look correct while
 * helios (which needs 64-bit rotations) was entirely wrong -- and devkitA64's
 * stricter diagnostics turned the same omission into a hard error. */
uint32_t hg_rol32(uint32_t v, int n);
uint32_t hg_ror32(uint32_t v, int n);
uint64_t hg_ror64(uint64_t v, int n);

/* Lowercase hex of `len` bytes into `out` (out needs 2*len+1 bytes). */
void hg_hex(const uint8_t *data, size_t len, char *out);
/* Uppercase hex, as used for the x-ss-stub header. */
void hg_hex_upper(const uint8_t *data, size_t len, char *out);
/* Parse a hex literal (must have even length). Returns 1 on success. */
int hg_unhex(const char *hex, uint8_t *out, size_t out_cap, size_t *out_len);

/* Standard base64 (no line breaks).  Returns the output length, or the
 * length that WOULD be produced when `out_cap` is too small (0 never means
 * "empty" for a non-empty input; test by comparing with `out_cap`). */
size_t hg_base64_encode(const uint8_t *data, size_t len, char *out, size_t out_cap);

/* Decode standard base64.  Returns 1 on success. */
int hg_base64_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* HG_CRYPTO_H */
