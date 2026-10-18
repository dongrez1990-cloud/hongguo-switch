// hg_lang.h -- the interface language, and the strings it selects.
//
// GPL-3.0-only.
//
// WHY A TABLE AND NOT A GETTEXT / JSON LAYER. This app has no translation infrastructure and does
// not need one: the interface's own text is a few dozen strings, all of them written by this project
// (the CONTENT -- drama titles, genre tags -- is upstream Chinese and is displayed as it arrives,
// never translated). A table of two columns with one function over it is smaller than any framework
// and, more importantly, it can be TESTED: hg_lang_test checks that every key has both languages and
// that no entry is empty, which is the failure that actually happens -- a key added in one language
// and forgotten in the other shows up as a blank label on a console screen, and a blank label reads
// as a rendering bug rather than a missing translation.
//
// THE DEFAULT IS CHINESE, at the user's explicit request: the interface language defaults to
// Chinese and English is opt-in. English is the
// fallback for anything a key is missing from, so a half-finished translation degrades to English
// rather than to nothing.
//
// ASCII-only source file -- which means the Chinese strings cannot be written as literals here. They
// are UTF-8 BYTE arrays, spelled out, because the build machine is GBK/936 and a non-ASCII source
// file leaks into the compiler as garbage. See the encoding note in hg_lang.c.

#ifndef HG_LANG_H
#define HG_LANG_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
  HG_LANG_ZH = 0,   /* Simplified Chinese -- the default */
  HG_LANG_EN = 1,
  /* "FOLLOW THE SYSTEM". Not a language: the caller resolves it through setsysGetSystemLanguage and calls
   * hg_lang_set with a real one. It is in the enum so hg_lang_parse can report the user's CHOICE distinctly
   * from its resolution -- "the label is in English" and "the label is in English because the system says so"
   * are different facts, and the boot log prints which one it was. */
  HG_LANG_AUTO = 2,
  HG_LANG_COUNT
};

/* A stable identifier per string. The order is the table's order; hg_lang_test walks the whole
 * range, so a key added without an entry fails the suite rather than showing blank. */
enum {
  /* Home screen */
  HG_T_HOME_SECTIONS = 0,   /* "%u cards in %u sections" */
  HG_T_REFRESH,
  HG_T_LOADING_HOME,
  HG_T_HOME_FAILED,
  HG_T_HOME_EMPTY,

  /* Detail screen */
  HG_T_EPISODES,            /* "%u episodes" */
  HG_T_DETAIL_HINT,         /* the key summary at the bottom */
  HG_T_LOADING_EPISODES,
  HG_T_EPISODES_FAILED,
  HG_T_EPISODES_EMPTY,
  HG_T_TAGS_FALLBACK_FAILED,

  /* Player overlay */
  HG_T_PLAYING,
  HG_T_PAUSED,
  HG_T_SEEKING,
  HG_T_LOADING_PLAN,
  HG_T_PLAYBACK_FAILED,

  /* Picture menu */
  HG_T_PICTURE,
  HG_T_ROW_BRIGHTNESS,
  HG_T_ROW_CONTRAST,
  HG_T_ROW_SATURATION,
  HG_T_ROW_GAMMA,
  HG_T_ROW_HUE,
  HG_T_ROW_QUALITY,
  HG_T_ROW_ASPECT,
  HG_T_ROW_MIRROR,
  HG_T_ROW_RESET,
  HG_T_MENU_HINT,
  /* ONLY ONE NEW KEY WAS NEEDED for the Chinese picture menu: the row labels and the value words
   * (On/Off/Auto/Crop/Stretch/not available) already had keys here -- HG_T_ROW_* and HG_T_VALUE_* below.
   * What was missing was the WIRING: the panel drew the C model's own English strings. This key is the
   * pending-quality marker, which had no string anywhere. */
  HG_T_MENU_APPLY,
  /* The player's own settings panel (the bar's 设置 chip). Four keys, because everything else that panel
   * shows is either a number or one of the VALUE_* words above. */
  HG_T_PSET_GENERAL,      /* 通用 */
  HG_T_PSET_SLEEP,        /* 定时关闭 */
  HG_T_PSET_BAR,          /* 固定显示进度条 */
  HG_T_PSET_MINUTES,      /* "%d 分钟" -- the sleep timer's value when it is on */
  HG_T_PSET_PLAYMODE,     /* 播放方式 -- what happens when an episode ends */
  HG_T_PSET_STOP,         /* 停在结尾 */
  HG_T_PSET_AUTONEXT,     /* 自动下一集 */
  HG_T_FAV_ADDED,         /* 已加入收藏 -- the detail screen's Y toggle says the RESULT */
  HG_T_FAV_REMOVED,       /* 已取消收藏 */
  /* The library screen (watch history + watchlist). */
  HG_T_LIB_HISTORY,       /* 历史记录 */
  HG_T_LIB_FAVORITES,     /* 我的收藏 */
  HG_T_LIB_NO_HISTORY,    /* 还没有观看记录 */
  HG_T_LIB_NO_FAVORITES,  /* 收藏夹是空的 */
  HG_T_EPISODE_N,         /* "第 %s 集" -- the line under a history card's title */
  HG_T_VALUE_ON,
  HG_T_VALUE_OFF,
  HG_T_VALUE_AUTO,
  HG_T_VALUE_CROP,
  HG_T_VALUE_STRETCH,
  HG_T_NOT_AVAILABLE,

  /* Settings screen */
  HG_T_SETTINGS_PLAYBACK,
  HG_T_SETTINGS_APPEARANCE,
  HG_T_SETTINGS_NETWORK,
  HG_T_SETTINGS_DIAGNOSTICS,
  HG_T_SETTINGS_ABOUT,
  HG_T_LANGUAGE,
  HG_T_LANGUAGE_ZH,
  HG_T_LANGUAGE_EN,

  /* Local history and the watchlist. These are the app's own screens, so both languages exist. The
   * age strings take one integer (%d). */
  HG_T_HOME,
  HG_T_HISTORY,
  HG_T_FAVORITES,
  HG_T_INBOX,
  HG_T_SETTINGS,
  /* The sidebar's skeleton placeholder, shown for a section that exists on the bar but whose screen
   * is not built yet. A section that silently does nothing reads as a bug; saying so reads as work
   * in progress, which is the truth. */
  HG_T_NOT_BUILT_YET,
  HG_T_BACK,
  /* "OK" -- the label borealis' own AppletFrame puts on its A-button action. The key reaches this app
   * UNTRANSLATED ("hints/ok") because borealis' i18n reads its catalogue from romfs and this build has none, and
   * showing a raw key to a user is worse than showing nothing. hg_hints.cpp rewrites such keys through this. */
  HG_T_OK,
  /* The other two keys borealis' defaults use. Same reason as HG_T_OK. */
  HG_T_QUIT,
  HG_T_CANCEL,
  HG_T_HISTORY_EMPTY,
  HG_T_FAVORITES_EMPTY,
  HG_T_FAVORITES_FULL,
  HG_T_ADDED_TO_FAV,
  HG_T_REMOVED_FROM_FAV,
  HG_T_CONTINUE,
  HG_T_WATCHED_TO,
  HG_T_REMOVE,
  HG_T_AGE_NOW,
  HG_T_AGE_MIN,
  HG_T_AGE_HOUR,
  HG_T_AGE_DAY,

  /* SETTINGS ROW LABELS. Kept at the END of the enum so the values above do not shift -- the table is
   * positional, so inserting in the middle silently renames every key after the insertion point. */
  HG_T_LB_RENDERER,
  HG_T_LB_HWDEC,
  HG_T_LB_AO,
  HG_T_LB_OVERLAY,
  HG_T_LB_MSGLVL,
  HG_T_LB_PMENU,
  HG_T_LB_SCRUB,
  HG_T_LB_QUALITY,
  HG_T_LB_UISCALE,
  HG_T_LB_TIERS,
  HG_T_LB_IMAGES,
  HG_T_LB_HOMELAYOUT,
  HG_T_LB_CELLW,
  HG_T_LB_EPMODE,
  HG_T_LB_EPCELLW,
  HG_T_LB_REFRESH,
  HG_T_LB_LANGUAGE,
  HG_T_LB_TIMEOUT,
  HG_T_LB_CONNECT,
  HG_T_LB_DNSCACHE,
  HG_T_LB_REDIRECT,
  HG_T_LB_TLS,
  HG_T_LB_PROXY,
  HG_T_LB_IMGTHREADS,
  HG_T_LB_OVERLAYON,
  HG_T_LB_RENDERCTX,
  HG_T_LB_LOADFILE,
  HG_T_LB_ADVANCED,
  HG_T_LB_FENCEWAIT,
  HG_T_LB_FETCH,
  HG_T_LB_SHOTPROBE,

  /* SELECTOR VALUES. Mapped by value, so "开" is one entry rather than one per row. */
  HG_T_OPT_ON,
  HG_T_OPT_OFF,
  HG_T_OPT_AUTO,
  HG_T_OPT_GRID,
  HG_T_OPT_LIST,
  HG_T_OPT_SOFTWARE,
  HG_T_OPT_NONE,
  HG_T_OPT_GPU,
  HG_T_OPT_CPU,
  HG_T_OPT_ASYNC,
  HG_T_OPT_SYNC,
  HG_T_OPT_DRAG,
  HG_T_OPT_LIVE,
  HG_T_OPT_VERBOSE,
  HG_T_OPT_INFO,

  HG_T_MORE,         /* the grid's last-slot "more" card */
  HG_T_OPT_DEC_HW,   /* hardware decode, the user-facing name for `auto` */
  HG_T_OPT_DEC_SW,   /* software decode, the user-facing name for `no` */

  /* THE SEARCH SCREEN. Its text is here rather than hardcoded for the same reason as every other
   * screen's: the interface is Chinese by default and English on request, and a hardcoded English
   * label is a screen that ignores the language setting. */
  HG_T_SEARCH,         /* "搜索" -- the screen's title and the home key's hint */
  HG_T_SEARCH_HINT,    /* "按 A 输入关键词" */
  HG_T_SEARCH_SUB,     /* the keyboard's own subtitle: "输入剧名（可拼音）" */
  HG_T_SEARCH_BUSY,    /* "正在搜索…" */
  HG_T_SEARCH_EMPTY,   /* "没有找到" -- a search that matched nothing, which is not an error */
  HG_T_SEARCH_SHOWING, /* "显示 %u 条" -- how many results this page carried */
  HG_T_SEARCH_TOTAL,   /* "（共 %lld 条）" -- and how many exist upstream, when it says */
  HG_T_LOADING,        /* "正在加载…" -- a page is in flight (shared by the search and category screens) */
  HG_T_SETTINGS_LICENSE,  /* 开源许可 -- the sixth section of the reference layout (C0) */
  HG_T_SEARCH_MIXED,   /* "（上游 %u 条 + 本站 %u 条）" -- the two result sources, named separately. See the
                        * search screen: upstream gives ten and no more, the local index gives the rest. */

  /* ---- THE PULL-UP MENUS ON THE CONTROL BAR (K16, the user's request) -------------------------------
   *
   * Each value chip used to show only its value, and left/right changed it in place. The user asked for the
   * console-player shape instead: press A on a chip and a small menu comes up to choose from -- and one chip
   * (volume) opens a DRAG BAR rather than a list.
   *
   * APPENDED AT THE END OF THE ENUM AND OF THE TABLE ON PURPOSE: the language table is positional, and this
   * project has already shipped a build where deleting rows from the middle shifted everything after them. Adding
   * at the end cannot move an existing entry.
   */
  HG_T_CTL_VOLUME,     /* 音量 */
  HG_T_CTL_SPEED,      /* 倍速 */
  HG_T_CTL_ZOOM,       /* 缩放 */
  HG_T_CTL_QUALITY,    /* 分辨率 */
  HG_T_CTL_DRAG,       /* "左右拖动" -- the hint under the volume bar */
  HG_T_PSET_PICTURE,   /* 画面设置 -- the panel row that opens the picture menu (the X menu, moved in here) */

  /* ---- WHAT TO SAY WHEN THE SERVICE REFUSES AN EPISODE (build175) ---------------------------------
   *
   * APPENDED AT THE END, for the same positional-table reason as the block above.
   *
   * WHY THIS KEY EXISTS: the console used to show a diagnostic -- "unavailable from this source after 20
   * attempts over 3 quality tiers. The server said HTTP 403-42 bytes-{...}". That is accurate and
   * useless to a viewer. The service refused the request for this device, which is a fact a person can
   * act on, so it is said in a sentence.
   *
   * WHAT IT DELIBERATELY DOES NOT SAY: nothing about a free range, a payment, an episode number or a
   * count. The measured boundary is per series (6, 11, 12, 13 and 50 across five series sampled on
   * 2026-10-16) and the responses carry pay_info.pay_type = 0 with a "full N episodes" rights string --
   * so the code does not know WHY, and wording that guessed would be wrong on the next series. It states
   * what was observed: the server refused it.
   *
   * IT ALSO DOES NOT MENTION LOGGING IN on its own authority -- it says the episode needs the service's
   * permission, which is exactly what a refusal means. */
  HG_T_EPISODE_REFUSED,
  /* The same refusal, for one hand-picked quality rather than a whole episode. Separate key because the
   * user chose that tier and the sentence has to be about the choice, not the episode. */
  HG_T_QUALITY_REFUSED,

  HG_T_COUNT
};

/* Set the language. Out-of-range values fall back to the default rather than being stored, so a
 * typo in a cfg file cannot leave the interface with no strings at all. */
void hg_lang_set(int lang);

/* The language in force. */
int hg_lang_get(void);

/* Parse a config value: "zh" / "cn" / "chinese" / "zh-hans" -> Chinese, "en" / "english" ->
 * English, anything else -> the default. Case-insensitive; returns the code, not a success flag. */
int hg_lang_parse(const char *value);

/* The name of a language in ITS OWN language, for the settings row: a Chinese speaker looking for
 * Chinese looks for a Chinese word. Never NULL. */
const char *hg_lang_name(int lang);

/* The string for a key in the language in force, falling back to English and then to an empty
 * string. NEVER returns NULL and never returns a dangling pointer (the table is static). */
const char *hg_tr(int key);

#ifdef __cplusplus
}
#endif

#endif /* HG_LANG_H */
