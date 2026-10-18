/*
 * Playback plan: turn a video-model response into everything mpv needs.
 *
 * GPL-3.0-only. Mirrors N3urda/hongguoTV server/vendor/hongguo.cjs:680-690 and
 * kotlin-tv/core MediaCrypto.kt (GPL-3.0), derived from woshishiq1/drpys
 * @22261ad (GPL-3.0). Combined work: GPL-3.0.
 *
 * This is deliberately thin. Everything hard already exists and is verified:
 *   - the spade URL decryptor  (hg_cenc.h, pinned to the reference)
 *   - the CENC content key     (hg_cenc.h, pinned to the reference)
 *   - the JSON reader          (hg_router.h)
 * What this file adds is the GLUE, which is where the interesting decision lives.
 *
 * THE DECISION: this project's original premise was that mpv would be reused
 * unchanged. That is only true if ffmpeg decrypts the CENC stream itself. It can
 * -- its MOV demuxer honours a `decryption_key` via --demuxer-lavf-o -- but that
 * was only ever confirmed by finding the option string inside the prebuilt
 * libavformat.a, never by playing anything. So the plan carries BOTH the option
 * string and the raw key: if the option path turns out not to work, the fallback
 * (feed mpv through a decrypting reader built on hg_cenc) needs the same key, and
 * discovering that at runtime is much worse than carrying 16 bytes.
 *
 * bytevc2 / bvc2 sources are skipped, as the reference does: the target player
 * cannot decode them, and offering one produces a black screen rather than an
 * error.
 *
 * WHERE THE FIELDS ACTUALLY ARE. Every path below was measured against a live
 * response (2026-xx, api5-normal-sinfonlineb.fqnovel.com), not inferred:
 *
 *   1. The response is DOUBLY ENCODED. The outer body is JSON, but
 *      data[<vid>].video_model is a JSON *string* that must be parsed a SECOND
 *      time, and video_list lives inside THAT. An earlier version of this file
 *      looked for video_list at the top level; it can never be found there, so
 *      the function returned "no model" for a perfectly good response.
 *
 *   2. Inside a video_list[] entry the entry-level keys are:
 *        main_url, backup_url, video_meta, encrypt_info, check_info,
 *        volume, quality_type, pallas_fid_labels, feature_id, gear_des_key
 *      Three fields the port previously read at the entry's top level are NOT
 *      there. Measured:
 *        codec_type  ->  video_meta.codec_type   (entry-level: ABSENT)
 *        spade_a     ->  encrypt_info.spade_a    (entry-level: ABSENT)
 *        key_seed    ->  DOES NOT EXIST ANYWHERE (main_url is already plaintext)
 *      Reading spade_a from the wrong place is silent: no CENC key, no error, and
 *      a black screen. That is why it is spelled out here.
 */

#ifndef HG_PLAY_H
#define HG_PLAY_H

#include <stddef.h>
#include <stdint.h>

/* For HG_AES_KEY128, which hg_spade_open_box() writes through. Included rather than repeating the
 * constant: the key length is a property of the cipher, and two definitions of it would be one too
 * many the first time it changed. */
#include "hg_aes.h"

#define HG_PLAY_URL_MAX   2048
#define HG_PLAY_HDR_MAX   512
#define HG_PLAY_DK_MAX    64
#define HG_PLAY_DEF_MAX   16      /* "1080p" / "360p" */
#define HG_PLAY_KID_MAX   64      /* measured: 32 hex chars */

/* Distinct outcomes. The caller has to be able to tell "upstream changed shape"
 * from "this response has no source I can decode" from "I cannot fit it in the
 * buffers", because the remedies are completely different. */
#define HG_PLAY_ERR_ARGS    (-1)  /* a NULL argument */
#define HG_PLAY_ERR_URL     (-2)  /* main_url absent, empty, or too long */
#define HG_PLAY_ERR_DECODE  (-3)  /* spade URL / key_seed decode failed */
#define HG_PLAY_ERR_KEY     (-4)  /* encrypt_info.spade_a did not yield a key */
#define HG_PLAY_ERR_NOMODEL (-5)  /* no outer/video_model/video_list at all */
#define HG_PLAY_ERR_SHAPE   (-6)  /* found the list, but entries are not objects */
#define HG_PLAY_ERR_CODEC   (-7)  /* every entry was bytevc2/bvc2 */
#define HG_PLAY_ERR_SMALL   (-8)  /* an inner string did not fit its buffer */

typedef struct {
  char url[HG_PLAY_URL_MAX];   /* resolved, ready to hand to mpv */
  char codec_type[32];         /* from video_meta; EMPTY when upstream omits it */
  int  has_codec_type;         /* 0 when video_meta.codec_type was absent */
  int  encrypted;              /* 1 when a CENC key was derived */
  uint8_t key[16];             /* the CENC content key, when encrypted */
  /* "User-Agent: ...,Referer: ..." -- the value for --http-header-fields */
  char header_fields[HG_PLAY_HDR_MAX];
  /* "decryption_key=<32 hex>" -- the value for --demuxer-lavf-o. Empty when the
   * stream is not encrypted. */
  char decryption_key[HG_PLAY_DK_MAX];

  /* ---- the SEPARATE AUDIO TRACK (build173) -------------------------------------------------------
   *
   * Empty on the old video_model route, whose entries were single progressive files. The play-info
   * route returns DASH with the audio as its own URL, and measured, that audio is a complete fMP4
   * (ftyp/moov/mvhd/trak/mvex/trex + its own sidx) that shares the video's CENC key -- so it is added
   * as a second track rather than being muxed or fetched by this layer. See player_view.cpp for where
   * mpv is told about it. */
  char audio_url[HG_PLAY_URL_MAX];

  /* --- what the caller needs beyond the URL ------------------------------- */

  /* quality_type is a NUMBER upstream (40 = 360p was the measured value, with
   * 30/20/10/1 for 480p/540p/720p/1080p). It is the stable machine-readable
   * ladder, where `definition` is a display label. 0 means "absent" -- upstream
   * never sends 0, and a caller picking by quality needs a value it can compare,
   * not an error path for a cosmetic field. */
  int     quality_type;
  int     has_quality_type;
  char    definition[HG_PLAY_DEF_MAX];   /* video_meta.definition, e.g. "360p" */
  int     vwidth, vheight;               /* video_meta.vwidth / vheight; 0 = absent */
  int     bitrate;                       /* video_meta.bitrate; 0 = absent */
  int64_t size_bytes;                    /* video_meta.size; 0 = absent */

  /* encrypt_info, carried because the CENC path may need it:
   *   encrypt == 0  -> the stream is NOT encrypted even though the entry has an
   *                    encrypt_info object; deriving a key would be wrong.
   *   kid           -> the key id, which a real license / unpacking path needs to
   *                    match against the MP4's `tenc`/`pssh` rather than assume.
   *                    Measured as 32 lowercase hex.
   *   encryption_method -- "cenc-aes-ctr" is the only value observed; any other
   *                    value is a scheme this decryptor does not implement, so it
   *                    is recorded rather than silently treated as CENC. */
  int  encrypt_flag;                     /* raw encrypt_info.encrypt; -1 = absent */
  char kid[HG_PLAY_KID_MAX];
  char encryption_method[24];            /* "cenc-aes-ctr", or empty */
} HgPlayPlan;

/* Build a plan from an already-chosen source. `key_seed` and `spade_a` may be
 * NULL or empty, meaning "not encrypted" -- the reference treats an empty seed
 * as "the URL is already plaintext".
 * Returns 0 on success. */
int hg_play_plan(const char *main_url, const char *key_seed, const char *spade_a,
                 const char *codec_type, HgPlayPlan *out);

/* Unwrap the doubly-encoded envelope, exposed so a caller that needs the SOURCE LIST rather
 * than one chosen source does not have to reproduce the unwrapping.
 *
 * The quality menu needs exactly that: hg_play_plan_from_model picks one entry and throws the
 * rest away, which is right for "play this episode" and useless for "offer the other
 * qualities". This hands back the inner document so hg_api_parse_video_model can be run over
 * all of it.
 *
 * `*out` points into either `json`'s own decoded-in-place buffer or a static buffer owned by
 * hg_play.c, so the caller must use it before the next call and must not free it. `*out_len`
 * receives its length.
 *
 * Returns 0 on success, or a negative HG_PLAY_ERR_*: HG_PLAY_ERR_SMALL when the inner document
 * is larger than the buffer (REFUSED, not truncated -- a truncated document still parses and
 * would silently yield a shorter video_list). */
int hg_play_model_inner(const char *json, size_t len, const char **out, size_t *out_len);

/* Pick the first decodable source from a video-model response and build a plan.
 * Handles BOTH shapes: the real doubly-encoded envelope (a string carrying
 * video_model, parsed a second time) and a bare inner object, because tests and
 * any future endpoint that inlines the model would present the latter.
 * Returns 0 on success, or a negative HG_PLAY_ERR_*. */
int hg_play_plan_from_model(const char *json, size_t len, HgPlayPlan *out);

/* ---- the play-info envelope (build173) -------------------------------------------------------------
 *
 * Everything mpv needs from ONE play-info response: the video URL, the SEPARATE audio URL, and the CENC
 * key. Kept apart from HgPlayPlan because the shape is genuinely different -- a plan is "one chosen
 * source out of a ladder", while this is "one episode, already chosen, in two pieces".
 *
 * `url` is the video stream and `audio_url` the audio stream. Both are complete, self-describing fMP4
 * files (measured: each carries ftyp/moov/mvhd/trak/mvex/trex plus its own sidx), so each is openable on
 * its own and the audio is meant to be added as a second track rather than muxed offline.
 *
 * `expires_at` is a Unix seconds value taken from the response; it is 0 when upstream omitted it. The
 * caller uses it to decide whether a cached address is still worth using. */
#define HG_PLAY_INFO_AUDIO_MAX 2048

typedef struct {
  char url[HG_PLAY_URL_MAX];              /* video main_url, ready for mpv */
  char backup_url[HG_PLAY_URL_MAX];       /* video backup_url; empty when absent */
  char audio_url[HG_PLAY_INFO_AUDIO_MAX]; /* audio main_url; empty when the response had no audio */
  char kid[HG_PLAY_KID_MAX];
  char definition[HG_PLAY_DEF_MAX];       /* "720p" / "480p" / "360p" -- echoed from the request */
  char codec_type[32];
  int vwidth, vheight;                    /* width/height from the response; 0 = absent */
  double duration;                        /* seconds; 0 when absent */
  long long expires_at;                   /* Unix seconds; 0 when absent */

  int encrypted;                          /* 1 when a CENC key was derived */
  uint8_t key[16];                        /* the derived CENC content key */
  /* "decryption_key=<32 hex>", the value for mpv's --demuxer-lavf-o. Empty when not encrypted.
   * The SAME key covers both tracks (measured: the response carries one key for video and audio). */
  char decryption_key[HG_PLAY_DK_MAX];
  /* "User-Agent: ...,Referer: ..." for --http-header-fields. */
  char header_fields[HG_PLAY_HDR_MAX];
} HgPlayInfo;

/* Parse a play-info response. Accepts both the real envelope ({"play_info":{...}}) and a bare
 * play_info object, so a caller that already unwrapped it, and the tests, can use the same entry point.
 *
 * Returns 0 on success, or a negative HG_PLAY_ERR_*: HG_PLAY_ERR_NOMODEL when there is no play_info at
 * all, HG_PLAY_ERR_URL when main_url is absent/empty/too long, and HG_PLAY_ERR_KEY when a
 * player_access_key was present but hg_spade_open_box() refused it (which is NOT fatal to the parse --
 * see the note in hg_play.c -- but is reported so the caller can log it). */
int hg_play_info_parse(const char *json, size_t len, HgPlayInfo *out);

/* Copy the parts of an HgPlayInfo that a player needs into an HgPlayPlan, so the mpv side keeps taking
 * ONE structure for both routes. The two shapes overlap almost entirely (URL, CENC key, header fields,
 * definition, codec); this exists so player_view.cpp does not grow a second code path for "the plan
 * came from the other endpoint", which is how two subtly different playback setups get created.
 *
 * `quality_type` is left at 0 with has_quality_type = 0: the play-info response carries no such field,
 * and inventing a number would make the quality menu claim a ladder this route does not have.
 * `vwidth`/`vheight` come from the response's width/height when present. Returns 0, or
 * HG_PLAY_ERR_ARGS on a NULL argument. */
int hg_play_plan_from_info(const HgPlayInfo *info, HgPlayPlan *out);

/* The buffer hg_play_plan_from_model needs for the whole OUTER response. Callers
 * that read a response body into a stack buffer must size that buffer with this,
 * since it bounds how much of the stream is examined at all. */
#define HG_PLAY_RESPONSE_MAX 65536

#endif /* HG_PLAY_H */
