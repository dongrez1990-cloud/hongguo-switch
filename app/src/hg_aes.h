/*
 * Standard AES-128 (FIPS 197) plus the CTR and CBC modes the media layer needs.
 *
 * GPL-3.0-only. Implemented from FIPS 197 / NIST SP 800-38A.
 *
 * WHY NOT mbedtls: same reasoning as hg_crypto.h -- the host verifier is TinyCC
 * on i386 and the target is aarch64, and keeping one self-contained C means the
 * code proven by the tests is exactly the code that ships.
 *
 * WHY NOT hg_aesv3: that type is the signing protocol's deliberately non-standard
 * transform (custom S-box, two rounds). Media encryption uses REAL AES; do not
 * substitute one for the other.
 *
 * The S-box is COMPUTED (multiplicative inverse in GF(2^8) + affine transform)
 * rather than transcribed as a 256-byte literal. A mistyped table entry is the
 * classic silent AES bug; deriving it means a wrong implementation fails the
 * FIPS 197 known-answer test immediately instead of producing plausible garbage.
 * The table is built once, lazily -- see the note on threading below.
 */

#ifndef HG_AES_H
#define HG_AES_H

#include <stddef.h>
#include <stdint.h>

#define HG_AES_BLOCK 16
#define HG_AES_KEY128 16

/* Expand a 16-byte key into 11 round keys (176 bytes). */
void hg_aes128_init(uint8_t round_keys[176], const uint8_t key[HG_AES_KEY128]);

/* Encrypt one 16-byte block. `in` and `out` may not overlap. */
void hg_aes128_encrypt_block(const uint8_t round_keys[176],
                             const uint8_t in[HG_AES_BLOCK],
                             uint8_t out[HG_AES_BLOCK]);

/* CTR mode (NIST SP 800-38A). `counter_block` is the full 16-byte big-endian
 * counter to start from; `skip` bytes of keystream are discarded before XORing,
 * so an arbitrary byte offset inside a stream can be decrypted without
 * decrypting the whole prefix.
 * Returns the number of bytes written (== len). */
size_t hg_aes_ctr(const uint8_t key[HG_AES_KEY128],
                  const uint8_t counter_block[HG_AES_BLOCK],
                  size_t skip,
                  const uint8_t *in, size_t len, uint8_t *out);

/* Add `delta` to a 128-bit big-endian counter in place, with carry.
 *
 * The CENC layer builds its counter as BigInteger(sample.iv) + blockIndex, where
 * sample.iv is SIXTEEN bytes (8 bytes taken from the auxiliary data followed by
 * eight zeros). That makes the auxiliary bytes the HIGH half of the counter and
 * the block index the LOW half -- an earlier helper here had them the other way
 * round, which would have decrypted every sample with a wrong counter. */
void hg_aes_counter_add(uint8_t counter[HG_AES_BLOCK], uint64_t delta);

/* CBC decrypt, no padding handling (the caller strips padding).
 * Returns the number of bytes written (a multiple of 16). */
size_t hg_aes128_cbc_decrypt(const uint8_t key[HG_AES_KEY128],
                             const uint8_t iv[HG_AES_BLOCK],
                             const uint8_t *in, size_t len, uint8_t *out);

#endif /* HG_AES_H */
