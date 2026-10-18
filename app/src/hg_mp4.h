/*
 * ISO-BMFF (MP4) header parsing for CENC playback.
 *
 * GPL-3.0-only. Ported from N3urda/hongguoTV MediaCrypto.kt (GPL-3.0), itself a
 * Kotlin adaptation of woshishiq1/drpys @22261ad (GPL-3.0). Combined work: GPL-3.0.
 *
 * This builds the HgCencPrepared table that hg_cenc_decrypt() walks: it finds the
 * encrypted video track, reads stsz/stsc/stco (+sti), saiz and saio, reconstructs
 * each sample's file offset, size and CTR IV, and rewrites the header so the
 * player sees plain `avc1`/`hvc1` instead of `encv` + sinf/frma.
 *
 * FAIL-CLOSED IS DELIBERATE. The reference throws on anything it cannot fully
 * account for (truncated sample index, mismatched auxiliary count, sub-sample
 * length that does not add up to the sample size, overlapping samples). Guessing
 * past a malformed index produces a player that decrypts the wrong bytes at the
 * wrong offsets, which looks like corruption rather than an error. Every one of
 * those checks is kept and surfaced as a distinct negative code.
 *
 * Error codes (all negative):
 *   HG_MP4_ERR_CONTAINER   not ftyp-then-moov at the front of the file
 *   HG_MP4_ERR_INDEX       moov size is out of range for the file
 *   HG_MP4_ERR_LAYOUT      a box tree that does not match the expected shape
 *   HG_MP4_ERR_TRUNCATED   an index that runs past its own box
 *   HG_MP4_ERR_UNSUPPORTED a layout this port does not handle
 *   HG_MP4_ERR_TOO_MANY    more samples than HG_CENC_MAX_SAMPLES
 *   HG_MP4_ERR_NO_SAMPLES  no usable encrypted track found
 */

#ifndef HG_MP4_H
#define HG_MP4_H

#include "hg_cenc.h"

#include <stddef.h>
#include <stdint.h>

#define HG_MP4_ERR_CONTAINER   (-1)
#define HG_MP4_ERR_INDEX       (-2)
#define HG_MP4_ERR_LAYOUT      (-3)
#define HG_MP4_ERR_TRUNCATED   (-4)
#define HG_MP4_ERR_UNSUPPORTED (-5)
#define HG_MP4_ERR_TOO_MANY    (-6)
#define HG_MP4_ERR_NO_SAMPLES  (-7)

/* Offset just past the moov box, or a negative error code.
 * `first` is the beginning of the file, `total` the file's full length. */
int hg_mp4_header_end(const uint8_t *first, size_t first_len, int64_t total);

/* Parse `data` (the header region: ftyp + moov, at least through moov) and fill
 * `prepared`. `header_out` receives a copy of `data` with the CENC metadata
 * neutralised and the original codec fourcc restored, so a player fed these
 * bytes sees a normal file; it must have room for `len` bytes.
 *
 * `key` may be NULL, meaning "no encryption": the header is still copied out and
 * zero samples are produced.
 *
 * Returns 0 on success or a negative HG_MP4_ERR_*. */
int hg_mp4_prepare(const uint8_t *data, size_t len, const uint8_t key[HG_AES_KEY128],
                   uint8_t *header_out, HgCencPrepared *prepared);

#endif /* HG_MP4_H */
