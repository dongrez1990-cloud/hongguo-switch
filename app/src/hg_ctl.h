/* hg_ctl.h -- the numbers behind the player's control bar: volume, speed, zoom.
 *
 * WHY THESE ARE NOT IN THE PICTURE MENU. The X menu is about the PICTURE (brightness, contrast, quality,
 * aspect); volume, speed and zoom are playback controls, and the reference layout the user gave puts them
 * on the bar at the bottom right for exactly that reason. They therefore get their own small model here.
 *
 * WHY THE ARITHMETIC IS IN C. Every one of these is a value with a range and a step, and every one of them
 * can be wrong SILENTLY: a volume that steps past 100 and wraps to 0, a speed ladder that skips 1.0x so the
 * user cannot get back to normal speed, a zoom whose factor and mpv property disagree by a factor of two.
 * All three are pure functions of an index and a delta, so they are unit-tested on the host, and the view
 * only draws what they return.
 *
 * ASCII-only source file.
 */
#ifndef HG_CTL_H
#define HG_CTL_H

/* ---- volume: mpv's `volume` property, an integer percentage ------------------- */

#define HG_CTL_VOLUME_MIN 0
#define HG_CTL_VOLUME_MAX 100
#define HG_CTL_VOLUME_STEP 5

/* Clamp a volume into range. Used on the cfg seed and on every write, because a bad cfg value would
 * otherwise be handed to mpv verbatim. */
int hg_ctl_volume_clamp(int volume);

/* One step of `delta` (+1/-1 or a multiple) from `volume`, clamped at both ends. Clamping rather than
 * wrapping is the whole point: a volume that wraps from 100 to 0 is a jump-scare. */
int hg_ctl_volume_step(int volume, int delta);

/* ---- speed: a LADDER, not a continuous value ---------------------------------
 *
 * The seven speeds every player offers, and the reason it is a ladder rather than 0.05 steps: the user has
 * to be able to get back to exactly 1.0x, and a continuous control that has been nudged is very hard to
 * return to the value it started at. Index 2 is 1.0x, which is also the default. */
#define HG_CTL_SPEED_COUNT 7
#define HG_CTL_SPEED_DEFAULT_INDEX 2

double hg_ctl_speed_at(int index);          /* the speed for an index, clamped into range */
int hg_ctl_speed_index(double speed);       /* the NEAREST index to a speed, for the cfg seed */
int hg_ctl_speed_step(int index, int delta);
void hg_ctl_speed_label(int index, char *buf, int bufsize);  /* "1.0x" */

/* ---- zoom: a ladder of FACTORS, whose mpv property is a power of two -----------
 *
 * mpv's `video-zoom` is defined as "scale the video by 2^value", so a factor of 2.0 means the property
 * value 1.0, not 2.0. Getting this wrong doubles the picture when the label says it doubled twice, and the
 * two functions below are the only place that conversion happens. */
#define HG_CTL_ZOOM_COUNT 4
#define HG_CTL_ZOOM_DEFAULT_INDEX 0

double hg_ctl_zoom_factor_at(int index);    /* 1.0, 1.25, 1.5, 2.0 */
int hg_ctl_zoom_index(double factor);       /* the nearest index to a factor */
int hg_ctl_zoom_step(int index, int delta);
void hg_ctl_zoom_label(int index, char *buf, int bufsize);   /* "1.25x" */
double hg_ctl_zoom_property(int index);     /* the value for mpv's `video-zoom` */

/* ---- the player's own settings (the panel the bar's 设置 chip opens) ------------
 *
 * WHAT IS HERE, AND WHY ONLY THIS. The reference layout's 通用 group has eight rows; this app can honestly
 * offer two of them today, and the rest are recorded in docs/待做清单.md with the reason each one cannot be
 * done (no shader chain on this render path, no highlight-bar data, no cloud history, and "what happens at the
 * end" needs episode switching, which is F1).
 *
 * The arithmetic is here rather than in the panel because both of these are values with a range and a step,
 * and a sleep timer that can be set to "0 minutes" or that wraps past its end is a silent bug.
 */

/* THE SLEEP TIMER: off, then 15-minute steps to 90. Index 0 is off. */
#define HG_CTL_SLEEP_COUNT 5
#define HG_CTL_SLEEP_DEFAULT_INDEX 0

int hg_ctl_sleep_minutes_at(int index);   /* 0, 15, 30, 60, 90 */
int hg_ctl_sleep_step(int index, int delta);

/* THE "ALWAYS SHOW THE PROGRESS BAR" SETTING: 0 or 1. A bool, but it is stepped with LEFT/RIGHT like every
 * other row, so it comes through the same clamp. */
int hg_ctl_flag_step(int value, int delta);

/* ---- the decode cache (C3), a ladder in MiB --------------------------------
 *
 * Index 0 means "mpv's own default", which is what this app has always used; the rest are the sizes a user
 * reaches for when the stream stutters. A LADDER rather than a number typed in, for the same reason the speed
 * ladder is one: 256 (meaning 256 MiB) and 256 (meaning something else) must not be two different things, and
 * the value that reaches mpv is computed in exactly one place.
 *
 * The sizes are what the app can afford in the console's memory: the software decode path already holds the
 * frame buffers, so this is a budget the user chooses rather than a number this app invents. */
#define HG_CTL_CACHE_COUNT 5
#define HG_CTL_CACHE_DEFAULT_INDEX 0

int hg_ctl_cache_mb_at(int index);    /* 0, 32, 64, 128, 256 -- 0 = leave mpv alone */
int hg_ctl_cache_step(int index, int delta);
int hg_ctl_cache_index(int mb);       /* the nearest index to a cfg value */

#endif /* HG_CTL_H */
