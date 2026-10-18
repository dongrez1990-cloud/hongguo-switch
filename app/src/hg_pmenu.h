/*
 * hg_pmenu -- the player's picture-adjustment menu, as pure logic.
 *
 * GPL-3.0-only.
 *
 * WHY THIS IS C AND NOT C++. The menu's rows, their ranges, what a keypress does
 * to them and what an out-of-range value collapses to are all arithmetic. This
 * project has already paid once for doing that arithmetic in a place that could
 * not be tested (a silent index error crashed the console with no message), so
 * the arithmetic lives here, where the host test suite can call it -- and the
 * C++ side is left with nothing but widgets and mpv calls.
 *
 * WHAT IT MODELS. wiliwili's in-player settings menu (fragment/player_setting.cpp
 * plus resources/xml/fragment/player_setting.xml) offers, in its equalizer group,
 * five sliders -- brightness, contrast, saturation, gamma, hue, each -100..100
 * with 0 as the neutral value -- plus an aspect selector, a mirror switch, and a
 * reset row that puts all five sliders back to the middle. Every one of those is
 * an mpv property rather than a filter this application has to implement:
 *
 *     brightness contrast saturation gamma hue    integers, -100..100
 *     aspect                                      keepaspect / video-aspect-override / panscan
 *     mirror                                      vf=hflip
 *
 * (setAspect / setMirror / setBrightness... at
 * upstream/wiliwili/wiliwili/source/view/mpv_core.cpp:1225-1296.)
 *
 * WHAT IT DELIBERATELY DOES NOT MODEL. The slider POSITION. wiliwili's sliders are
 * continuous 0..1 whose value is (position * 200 - 100); this menu has no slider
 * widget at all -- see the note in player_view.cpp about why a brls::Slider must
 * not be put on this screen -- so the value IS the state and the display is built
 * from it. That also means there is no float anywhere: -100..100 is an integer and
 * every step is exact.
 *
 * ASCII-only source file.
 */

#ifndef HG_PMENU_H
#define HG_PMENU_H

#ifdef __cplusplus
extern "C" {
#endif

/* The rows, in the order they are shown. The two ends are not interchangeable:
 * HG_PMENU_ROW_FIRST_ADJUST / _LAST_ADJUST bound the numeric rows, and
 * hg_pmenu_row_is_adjust() is how the renderer decides between "draw a number" and
 * "draw one of the named values". */
enum {
  HG_PMENU_ROW_BRIGHTNESS = 0,
  HG_PMENU_ROW_CONTRAST,
  HG_PMENU_ROW_SATURATION,
  HG_PMENU_ROW_GAMMA,
  HG_PMENU_ROW_HUE,
  HG_PMENU_ROW_QUALITY,
  HG_PMENU_ROW_ASPECT,
  HG_PMENU_ROW_MIRROR,
  HG_PMENU_ROW_RESET,
  HG_PMENU_ROW_COUNT,

  HG_PMENU_ROW_FIRST_ADJUST = HG_PMENU_ROW_BRIGHTNESS,
  HG_PMENU_ROW_LAST_ADJUST = HG_PMENU_ROW_HUE
};

/* THE QUALITY ROW'S VALUE LIVES OUTSIDE THIS STRUCT.
 *
 * Every other row owns its value: brightness is a number, aspect is one of three, mirror is on
 * or off. Quality is different in a way that matters -- the list of tiers is not known until
 * the player has fetched the video model, and the model's five tiers (MEASURED:
 * quality_type 40/30/20/10/1 = 360p/480p/540p/720p/1080p) come from the NETWORK, not from
 * here.
 *
 * So this row stores only which tier of a list the caller maintains is selected, and the
 * caller renders the label. Keeping the list out of this struct is what lets the arithmetic
 * stay testable: this file's tests cover "the index wraps and clamps", and the ladder's own
 * ordering and labelling is hg_quality's job, checked against the five measured rows there.
 *
 * `qualityCount` is 0 until the ladder arrives, and a count of 0 means the row renders as
 * unavailable rather than as "quality 0" -- the difference between an empty menu and a wrong
 * one. */
typedef struct {
  int open;    /* 0 = closed. A CLOSED MENU MUST NOT REACH A KEYSTROKE. */
  int row;     /* the highlighted row, always in [0, HG_PMENU_ROW_COUNT) */
  int values[HG_PMENU_ROW_LAST_ADJUST + 1];
  int aspect;  /* HG_PMENU_ASPECT_* */
  int mirror;  /* 0 = off, 1 = vf=hflip */

  /* The quality row: an index into the caller's ladder, and how long that ladder is. */
  int qualityIndex;
  int qualityCount;

  /* WHICH TIER IS ACTUALLY PLAYING.
   *
   * The user asked for the two-step: "左右只移光标、A 才生效", because changing a tier reloads the stream and a
   * cursor that reloads on every press is unusable. So `qualityIndex` is the CURSOR and this is the tier in
   * force; the row shows the cursor and marks it pending (HG_T_MENU_APPLY) until the two agree. The CALLER owns
   * the number, because it is the player that knows which tier is playing. */
  int qualityApplied;
} HgPMenu;

/* mpv's own range for the five picture properties, and wiliwili's clamp
 * (mpv_core.cpp:1263-1296 clamps to exactly this pair). */
#define HG_PMENU_VALUE_MIN (-100)
#define HG_PMENU_VALUE_MAX (100)

/* Aspect modes. These are OUR names for the three states wiliwili expresses with
 * the strings "-1"/"-2"/"-3": the mapping to mpv's keepaspect /
 * video-aspect-override / panscan triple is hg_pmenu_aspect_props(), so the
 * mapping is testable rather than buried in a switch inside the UI. */
enum {
  HG_PMENU_ASPECT_AUTO = 0,   /* keep aspect, mpv's own guess: the default */
  HG_PMENU_ASPECT_CROP,       /* keep aspect, panscan 1.0: fill, cut the overflow */
  HG_PMENU_ASPECT_STRETCH,    /* no aspect: fill exactly, distort if it must */
  HG_PMENU_ASPECT_COUNT
};

/* Where a value came from, for the log line that records it. Applying on open as
 * well as on change is what makes the cfg file an arm that can be flipped without
 * a rebuild -- and a line saying which arm was in force is the difference between
 * "the property did not change" and "the property was never meant to". */
enum {
  HG_PMENU_SRC_DEFAULT = 0,   /* the built-in neutral values */
  HG_PMENU_SRC_CFG,           /* hongguo.cfg, read when the menu was built */
  HG_PMENU_SRC_USER           /* the user moved a row */
};

/* Neutral defaults: every picture property 0, aspect auto, mirror off. This is
 * exactly the state the player was in before this menu existed, which is what
 * makes "the menu changed nothing" true until a row is touched. */
void hg_pmenu_reset(HgPMenu *m);

/* Clamp one raw value into mpv's range. Out-of-range values are not rejected, they
 * are clamped: mpv documents brightness above 100 as undefined, and a cfg file a
 * human edited should not be able to put the picture into an undefined state. */
int hg_pmenu_clamp(int v);

/* Row navigation. Both directions WRAP, which is what a short list wants: from the
 * top row, up lands on the last one. Returns 1 when the highlighted row changed --
 * a caller that logs only on change uses that, because a held UP that has stopped
 * moving would otherwise repeat a line every frame. */
int hg_pmenu_move(HgPMenu *m, int delta);

/* Apply one keypress to the highlighted row. `dir` is +1 or -1. `fine` selects the
 * small step (1) instead of the normal one (5).
 *
 * Returns 1 when a VALUE changed, 0 when the key did nothing to this row -- which
 * is the normal outcome at a clamp bound, for MIRROR (a switch: either direction
 * toggles it) and for RESET when there is nothing to reset. The caller logs the
 * difference, so "the key did nothing" and "the key was never seen" stay
 * distinguishable. */
int hg_pmenu_adjust(HgPMenu *m, int dir, int fine);

/* The normal and fine step sizes, exposed so the on-screen hint and the tests both
 * state them rather than each hard-coding their own idea of them. */
int hg_pmenu_step_normal(void);
int hg_pmenu_step_fine(void);

/* 1 when the row holds a number (brightness..hue), 0 when it holds one of a set of
 * named values (aspect, mirror) or is an action (reset). */
int hg_pmenu_row_is_adjust(int row);

/* The row's label, or NULL when row is out of range. Static storage: the caller
 * must not free it. */
const char *hg_pmenu_row_label(int row);

/* THE SAME LABEL, AS A LANGUAGE KEY.
 *
 * WHY BOTH EXIST. The English string above is what goes into hongguo.log, where this project keeps every
 * message in one language. The key is what the PANEL draws, so the menu speaks Chinese like the rest of the
 * app. Two different jobs for the same row, and the mapping is one table the host tests check (every row must
 * have a key that resolves to a real string).
 *
 * Returns the HG_T_ROW_* key, or -1 when the row has no label. */
int hg_pmenu_row_label_key(int row);

/* The KEY for a row's VALUE when that value is a word rather than a number, or -1 when the caller should use
 * hg_pmenu_row_value() instead (numbers, "3 of 5", the reset hint). */
int hg_pmenu_row_value_key(const HgPMenu *m, int row);

/* Format the row's current value into buf. Always NUL-terminates and always writes
 * something ("?" for an out-of-range row), so a mis-indexed call shows up as a
 * question mark on screen instead of leaving stale text in a label. */
const char *hg_pmenu_row_value(const HgPMenu *m, int row, char *buf, int bufsize);

/* What mpv should be told for the current aspect. Always fills all three, so a
 * caller cannot change one of the triple and leave the other two describing the
 * previous mode -- the bug that makes "stretch" leave a stretched picture with
 * black bars around it. */
void hg_pmenu_aspect_props(const HgPMenu *m, const char **keepaspect,
                           const char **aspectOverride, const char **panscan);

/* ---- the vf CHAIN, AND WHY THE FIVE VALUES NEED ONE (I3) --------------------
 *
 * REPORTED: changing brightness (and the other four) did nothing to the picture, while the log showed the write
 * succeeding and reading back the value asked for.
 *
 * MEASURED, from this app's own log and source: it renders through MPV_RENDER_API_TYPE_SW with vo=libmpv, and the
 * cfg in force has hwdec=no -- so the decode path is NOT bypassing anything (the handover's guess about
 * hardware decoding was wrong, as its own note suspected). The five properties
 * (brightness/contrast/saturation/gamma/hue) are mpv's "video equalizer", which the GPU video chain applies as
 * SHADER parameters; the software path has no such stage, so a property that is accepted and reads back
 * correctly can still change nothing on screen. That is exactly the symptom.
 *
 * THE FIX IS TO SAY THE SAME THING AS FILTERS: ffmpeg's `eq` and `hue` filters are ordinary video filters, so
 * they run on every path including this one. hg_pmenu_vf() builds the whole chain in ONE string -- one writer,
 * because writing `vf` twice would silently drop the first half (the mirror already wrote it).
 *
 * THE CONVERSION, from each tool's own neutral point:
 *     mpv brightness  -100..100, neutral 0  ->  eq brightness  -1..1,   neutral 0    (/100)
 *     mpv contrast    -100..100, neutral 0  ->  eq contrast     ..,     neutral 1    (1 + v/100)
 *     mpv saturation  -100..100, neutral 0  ->  eq saturation   0..3,   neutral 1    (1 + v/100)
 *     mpv gamma       -100..100, neutral 0  ->  eq gamma        0.1..10 neutral 1    (1 + v/100)
 *     mpv hue         -100..100, neutral 0  ->  hue h          -180..180            (v * 1.8)
 *
 * `player.eq.mode=props` keeps the old behaviour (properties only) for one cfg line, which is how every risky
 * change in this project ships. */
void hg_pmenu_vf(const HgPMenu *m, char *buf, int bufsize);
const char *hg_pmenu_mirror_filter(const HgPMenu *m);

#ifdef __cplusplus
}
#endif

#endif /* HG_PMENU_H */
