// hg_lang implementation. See hg_lang.h for why this is a table and not a framework.
//
// HOW THE CHINESE STRINGS ARE STORED, AND WHY THIS WAY.
//
// This project's build machine is GBK/936 and a non-ASCII source file arrives at the compiler as
// garbage -- that has cost this port several rounds already (Chinese in a comment, Chinese in a
// string literal, and a full-width bracket that produced "missing terminating ' character" from a
// line that looked perfectly fine).
//
// So every non-ASCII string here is written as `\xNN` byte escapes, which is pure ASCII source that
// the C concatenation rule glues into one string literal. The one trap with that notation is that a
// hex escape is GREEDY: "\xE4\xB8\xAD" is fine because the next character is another backslash, but
// a trailing space or letter after a byte escape would be swallowed into it. Every entry below
// therefore ends its last escape at a string boundary or before another backslash, which is why the
// literals look chopped up.
//
// hg_lang_test decodes each entry and checks it is well-formed UTF-8, which is the check that turns
// "I typed the right bytes" into something the host suite proves.
//
// ASCII-only source file.

#include "hg_lang.h"

#include <stdio.h>
#include <string.h>

/* Chinese, English. A NULL Chinese entry falls back to English. */
typedef struct {
  const char *zh;   /* UTF-8 bytes via \xNN escapes; NULL = use English */
  const char *en;
} LangPair;

static const LangPair TABLE[HG_T_COUNT] = {
    /* --- home screen --- */
    /* "%u cards in %u sections" */
    {"\xE5\x8D\xA1\xE7\x89\x87" " / " "\xE5\x88\x86\xE5\x8C\xBA", "cards / sections"},
    {"\xE5\x88\xB7\xE6\x96\xB0", "Refresh"},
    {"\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x8A\xA0\xE8\xBD\xBD\xE9\xA6\x96\xE9\xA1\xB5\xE2\x80\xA6",
     "Loading home page..."},
    {"\xE9\xA6\x96\xE9\xA1\xB5\xE5\x8A\xA0\xE8\xBD\xBD\xE5\xA4\xB1\xE8\xB4\xA5",
     "Home failed"},
    {"\xE9\xA6\x96\xE9\xA1\xB5\xE6\xB2\xA1\xE6\x9C\x89\xE5\x86\x85\xE5\xAE\xB9",
     "The home page is empty"},

    /* --- detail screen --- */
    {"\xE9\x9B\x86", "episodes"},
    {"\xE6\x8C\x89 A \xE6\x92\xAD\xE6\x94\xBE   \xE6\x8C\x89 B \xE8\xBF\x94\xE5\x9B\x9E   "
     "\xE6\x8C\x89 + \xE9\x80\x80\xE5\x87\xBA",
     "A plays the highlighted episode    B goes back    + exits"},
    {"\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x8A\xA0\xE8\xBD\xBD\xE9\x9B\x86\xE6\x95\xB0\xE2\x80\xA6",
     "Loading episodes..."},
    {"\xE9\x9B\x86\xE6\x95\xB0\xE5\x8A\xA0\xE8\xBD\xBD\xE5\xA4\xB1\xE8\xB4\xA5",
     "Episodes failed"},
    {"\xE6\xB2\xA1\xE6\x9C\x89\xE9\x9B\x86\xE6\x95\xB0", "No episodes"},
    {"\xE6\x9C\xAA\xE6\x8F\x90\xE4\xBE\x9B\xE7\xAE\x80\xE4\xBB\x8B", "No synopsis provided"},

    /* --- player overlay --- */
    {"\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x92\xAD\xE6\x94\xBE", "PLAYING"},
    {"\xE5\xB7\xB2\xE6\x9A\x82\xE5\x81\x9C", "PAUSED"},
    {"\xE5\xBF\xAB\xE8\xBF\x9B\xE9\x80\x80", "SEEK"},
    {"\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x87\x86\xE5\xA4\x87\xE6\x92\xAD\xE6\x94\xBE\xE2\x80\xA6",
     "Preparing playback..."},
    {"\xE6\x92\xAD\xE6\x94\xBE\xE5\xA4\xB1\xE8\xB4\xA5", "Playback failed"},

    /* --- picture menu --- */
    {"\xE7\x94\xBB\xE9\x9D\xA2", "Picture"},
    {"\xE4\xBA\xAE\xE5\xBA\xA6", "Brightness"},
    {"\xE5\xAF\xB9\xE6\xAF\x94\xE5\xBA\xA6", "Contrast"},
    {"\xE9\xA5\xB1\xE5\x92\x8C\xE5\xBA\xA6", "Saturation"},
    {"\xE4\xBC\xBD\xE9\xA9\xAC", "Gamma"},
    {"\xE8\x89\xB2\xE7\x9B\xB8", "Hue"},
    {"\xE6\xB8\x85\xE6\x99\xB0\xE5\xBA\xA6", "Quality"},
    {"\xE7\x94\xBB\xE9\x9D\xA2\xE6\xAF\x94\xE4\xBE\x8B", "Aspect"},
    {"\xE9\x95\x9C\xE5\x83\x8F", "Mirror"},
    {"\xE5\x85\xA8\xE9\x83\xA8\xE9\x87\x8D\xE7\xBD\xAE", "Reset all"},
    /* THE HINT NAMES THE COMMIT NOW. The quality row is the one row where A is not merely "close": it applies
     * the tier the cursor is on (the user's two-step, §3.1). A hint that only said "A closes" would leave the
     * user believing that a tier they moved the cursor to is playing. */
    {"\xE6\x8C\x89 B \xE6\x88\x96 A \xE5\x85\xB3\xE9\x97\xAD   -   \xE4\xB8\x8A\xE4\xB8\x8B"
     "\xE9\x80\x89\xE8\xA1\x8C   -   \xE5\xB7\xA6\xE5\x8F\xB3\xE8\xB0\x83\xE6\x95\xB4   -   "
     "\xE6\xB8\x85\xE6\x99\xB0\xE5\xBA\xA6\xE8\xA1\x8C\xE6\x8C\x89 A \xE7\x94\x9F\xE6\x95\x88",
     "B or A closes   -   Up/Down row   -   Left/Right change   -   A applies quality"},
    /* The pending-quality marker (see hg_lang.h): everything else the picture menu shows already had a
     * key -- the panel simply was not using them. */
    {"\xE6\x8C\x89 A \xE5\xBA\x94\xE7\x94\xA8", "press A to apply"},
    /* --- the player's own settings panel (the bar's 设置 chip) --- */
    {"\xE9\x80\x9A\xE7\x94\xA8", "General"},
    {"\xE5\xAE\x9A\xE6\x97\xB6\xE5\x85\xB3\xE9\x97\xAD", "Sleep timer"},
    {"\xE5\x9B\xBA\xE5\xAE\x9A\xE6\x98\xBE\xE7\xA4\xBA\xE8\xBF\x9B\xE5\xBA\xA6\xE6\x9D\xA1", "Always show progress"},
    {"%d \xE5\x88\x86\xE9\x92\x9F", "%d min"},
    {"\xE6\x92\xAD\xE6\x94\xBE\xE6\x96\xB9\xE5\xBC\x8F", "Play mode"},
    {"\xE5\x81\x9C\xE5\x9C\xA8\xE7\xBB\x93\xE5\xB0\xBE", "Stop at the end"},
    {"\xE8\x87\xAA\xE5\x8A\xA8\xE4\xB8\x8B\xE4\xB8\x80\xE9\x9B\x86", "Auto-next"},
    /* The detail screen's watchlist toggle reports the RESULT, not the action. */
    {"\xE5\xB7\xB2\xE5\x8A\xA0\xE5\x85\xA5\xE6\x94\xB6\xE8\x97\x8F", "Added to your list"},
    {"\xE5\xB7\xB2\xE5\x8F\x96\xE6\xB6\x88\xE6\x94\xB6\xE8\x97\x8F", "Removed from your list"},
    /* --- the library screen (watch history + watchlist) --- */
    {"\xE5\x8E\x86\xE5\x8F\xB2\xE8\xAE\xB0\xE5\xBD\x95", "History"},
    {"\xE6\x88\x91\xE7\x9A\x84\xE6\x94\xB6\xE8\x97\x8F", "My list"},
    {"\xE8\xBF\x98\xE6\xB2\xA1\xE6\x9C\x89\xE8\xA7\x82\xE7\x9C\x8B\xE8\xAE\xB0\xE5\xBD\x95", "No watch history yet"},
    {"\xE6\x94\xB6\xE8\x97\x8F\xE5\xA4\xB9\xE6\x98\xAF\xE7\xA9\xBA\xE7\x9A\x84", "Your list is empty"},
    {"\xE7\xAC\xAC %s \xE9\x9B\x86", "episode %s"},
    {"\xE5\xBC\x80", "On"},
    {"\xE5\x85\xB3", "Off"},
    {"\xE8\x87\xAA\xE5\x8A\xA8", "Auto"},
    {"\xE8\xA3\x81\xE5\x89\xAA", "Crop"},
    {"\xE6\x8B\x89\xE4\xBC\xB8", "Stretch"},
    {"\xE4\xB8\x8D\xE5\x8F\xAF\xE7\x94\xA8", "(not available)"},

    /* --- settings screen --- */
    {"\xE6\x92\xAD\xE6\x94\xBE\xE8\xAE\xBE\xE7\xBD\xAE", "Playback settings"},
    {"\xE7\x95\x8C\xE9\x9D\xA2\xE8\xAE\xBE\xE7\xBD\xAE", "Screen settings"},
    {"\xE5\x85\xB6\xE4\xBB\x96\xE8\xAE\xBE\xE7\xBD\xAE", "Other settings"},
    {"\xE5\xAE\x9E\xE7\x94\xA8\xE5\xB7\xA5\xE5\x85\xB7", "Tools"},
    {"\xE5\x85\xB3\xE4\xBA\x8E\xE6\x9B\xB4\xE5\xA4\x9A", "About this build"},
    {"\xE8\xAF\xAD\xE8\xA8\x80", "Language"},
    {"\xE4\xB8\xAD\xE6\x96\x87", "Chinese"},
    {"English", "English"},

    /* --- local history and the watchlist. These are the app's OWN screens, so both languages
     * exist. The age rows take one integer (%d). --- */
    {"\xE4\xB8\xBB\xE9\xA1\xB5", "Home"},                                  /* 主页 */
    {"\xE5\x8E\x86\xE5\x8F\xB2\xE8\xAE\xB0\xE5\xBD\x95", "History"},       /* 历史记录 */
    {"\xE6\x94\xB6\xE8\x97\x8F", "Favorites"},                             /* 收藏 */
    {"\xE4\xBF\xA1\xE6\x81\xAF", "Inbox"},                                 /* 信息 */
    {"\xE8\xAE\xBE\xE7\xBD\xAE", "Settings"},                              /* 设置 */
    {"\xE8\xBF\x99\xE4\xB8\x80\xE9\xA1\xB5\xE8\xBF\x98\xE6\xB2\xA1\xE5\x81\x9A\xE5\xA5\xBD",
     "This screen is not built yet"},                                     /* 这一页还没做好 */
    {"\xE8\xBF\x94\xE5\x9B\x9E", "Back"},                                  /* 返回 */
    {"\xE7\xA1\xAE\xE5\xAE\x9A", "OK"},                                  /* 确定 */
    {"\xE9\x80\x80\xE5\x87\xBA", "Quit"},                                /* 退出 */
    {"\xE5\x8F\x96\xE6\xB6\x88", "Cancel"},                              /* 取消 */
    {"\xE8\xBF\x98\xE6\xB2\xA1\xE6\x9C\x89\xE8\xA7\x82\xE7\x9C\x8B\xE8\xAE\xB0\xE5\xBD\x95",
     "Nothing watched yet"},                                               /* 还没有观看记录 */
    {"\xE8\xBF\x98\xE6\xB2\xA1\xE6\x9C\x89\xE6\x94\xB6\xE8\x97\x8F",
     "Nothing saved yet"},                                                 /* 还没有收藏 */
    {"\xE6\x94\xB6\xE8\x97\x8F\xE5\xB7\xB2\xE6\xBB\xA1", "The list is full"},  /* 收藏已满 */
    {"\xE5\xB7\xB2\xE6\x94\xB6\xE8\x97\x8F", "Added to favorites"},       /* 已收藏 */
    {"\xE5\xB7\xB2\xE5\x8F\x96\xE6\xB6\x88\xE6\x94\xB6\xE8\x97\x8F", "Removed"}, /* 已取消收藏 */
    {"\xE7\xBB\xA7\xE7\xBB\xAD\xE8\xA7\x82\xE7\x9C\x8B", "Continue"},     /* 继续观看 */
    {"\xE7\x9C\x8B\xE5\x88\xB0", "Watched to"},                           /* 看到 */
    {"\xE7\xA7\xBB\xE9\x99\xA4", "Remove"},                               /* 移除 */
    {"\xE5\x88\x9A\xE5\x88\x9A", "just now"},                              /* 刚刚 */
    {"%d \xE5\x88\x86\xE9\x92\x9F\xE5\x89\x8D", "%d min ago"},            /* N 分钟前 */
    {"%d \xE5\xB0\x8F\xE6\x97\xB6\xE5\x89\x8D", "%d h ago"},              /* N 小时前 */
    {"%d \xE5\xA4\xA9\xE5\x89\x8D", "%d d ago"},                          /* N 天前 */

    /* --- settings row labels --- */
    {"\xE6\xB8\xB2\xE6\x9F\x93\xE5\x99\xA8", "Renderer"},                 /* 渲染器 */
    {"\xE8\xA7\xA3\xE7\xA0\x81\xE6\x96\xB9\xE5\xBC\x8F", "Video decode"}, /* 解码方式 */
    {"\xE9\x9F\xB3\xE9\xA2\x91\xE8\xBE\x93\xE5\x87\xBA", "Audio output"}, /* 音频输出 */
    {"\xE6\x92\xAD\xE6\x94\xBE\xE5\x99\xA8\xE6\xA0\x87\xE8\xAF\x86", "Player overlay"}, /* 播放器标识 */
    {"mpv \xE6\x97\xA5\xE5\xBF\x97", "mpv log level"},                    /* mpv 日志 */
    {"\xE7\x94\xBB\xE9\x9D\xA2\xE8\x8F\x9C\xE5\x8D\x95", "Picture menu"}, /* 画面菜单 */
    {"\xE6\x8B\x96\xE5\x8A\xA8\xE6\x96\xB9\xE5\xBC\x8F", "Seek style"},   /* 拖动方式 */
    {"\xE6\xB8\x85\xE6\x99\xB0\xE5\xBA\xA6\xE8\x8F\x9C\xE5\x8D\x95", "Quality row"}, /* 清晰度菜单 */
    {"\xE7\x95\x8C\xE9\x9D\xA2\xE5\x88\x86\xE8\xBE\xA8\xE7\x8E\x87", "UI resolution"}, /* 界面分辨率 */
    {"\xE9\xA6\x96\xE9\xA1\xB5\xE5\x88\x86\xE5\x8C\xBA", "Home sections"}, /* 首页分区 */
    {"\xE5\xB0\x81\xE9\x9D\xA2\xE5\x9B\xBE", "Cover images"},             /* 封面图 */
    {"\xE9\xA6\x96\xE9\xA1\xB5\xE5\xB8\x83\xE5\xB1\x80", "Home layout"},  /* 首页布局 */
    {"\xE5\x8D\xA1\xE7\x89\x87\xE5\xAE\xBD\xE5\xBA\xA6", "Poster width"}, /* 卡片宽度 */
    {"\xE9\x80\x89\xE9\x9B\x86\xE5\xB8\x83\xE5\xB1\x80", "Episode picker"}, /* 选集布局 */
    {"\xE9\x80\x89\xE9\x9B\x86\xE6\x8C\x89\xE9\x92\xAE\xE5\xAE\xBD", "Episode button width"},
    {"\xE5\x88\xB7\xE6\x96\xB0\xE6\x8C\x89\xE9\x92\xAE", "Refresh button"}, /* 刷新按钮 */
    {"\xE8\xAF\xAD\xE8\xA8\x80", "Language"},                              /* 语言 */
    {"\xE8\xAF\xB7\xE6\xB1\x82\xE8\xB6\x85\xE6\x97\xB6", "Request timeout"}, /* 请求超时 */
    {"\xE8\xBF\x9E\xE6\x8E\xA5\xE8\xB6\x85\xE6\x97\xB6", "Connect timeout"}, /* 连接超时 */
    {"DNS \xE7\xBC\x93\xE5\xAD\x98", "DNS cache"},                          /* DNS 缓存 */
    {"\xE8\xB7\x9F\xE9\x9A\x8F\xE9\x87\x8D\xE5\xAE\x9A\xE5\x90\x91", "Follow redirects"},
    {"\xE6\xA0\xA1\xE9\xAA\x8C\xE8\xAF\x81\xE4\xB9\xA6", "Verify certificate"}, /* 校验证书 */
    {"\xE4\xBB\xA3\xE7\x90\x86", "Proxy"},                                  /* 代理 */
    {"\xE5\xB9\xB6\xE5\x8F\x91\xE5\x8F\x96\xE5\x9B\xBE", "Image fetches at once"}, /* 并发取图 */
    {"\xE7\xBB\x98\xE5\x88\xB6\xE7\x95\x8C\xE9\x9D\xA2", "Draw the UI"},   /* 绘制界面 */
    {"\xE5\x88\x9B\xE5\xBB\xBA\xE6\xB8\xB2\xE6\x9F\x93\xE4\xB8\x8A\xE4\xB8\x8B\xE6\x96\x87", "Create render context"},
    {"\xE6\x89\x93\xE5\xBC\x80\xE6\xB5\x81", "Open the stream"},           /* 打开流 */
    {"\xE9\xAB\x98\xE7\xBA\xA7\xE6\x8E\xA7\xE5\x88\xB6", "Advanced control"}, /* 高级控制 */
    {"\xE7\xAD\x89\xE5\xBE\x85\xE6\x96\xB9\xE5\xBC\x8F", "Fence wait"},    /* 等待方式 */
    {"\xE9\xA6\x96\xE9\xA1\xB5\xE5\x8F\x96\xE6\x95\xB0", "Home fetch"},    /* 首页取数 */
    {"\xE6\x88\xAA\xE5\x9B\xBE\xE6\x8E\xA2\xE9\x92\x88", "Screenshot probe"}, /* 截图探针 */

    /* --- selector values --- */
    {"\xE5\xBC\x80", "On"},                                                /* 开 */
    {"\xE5\x85\xB3", "Off"},                                               /* 关 */
    {"\xE8\x87\xAA\xE5\x8A\xA8", "Auto"},                                  /* 自动 */
    {"\xE7\xBD\x91\xE6\xA0\xBC", "Grid"},                                  /* 网格 */
    {"\xE5\x88\x97\xE8\xA1\xA8", "List"},                                  /* 列表 */
    {"\xE8\xBD\xAF\xE8\xA7\xA3", "Software"},                              /* 软解 */
    {"\xE6\x97\xA0", "None"},                                              /* 无 */
    {"GPU", "GPU"},
    {"CPU", "CPU"},
    {"\xE5\xBC\x82\xE6\xAD\xA5", "Async"},                                 /* 异步 */
    {"\xE5\x90\x8C\xE6\xAD\xA5", "Sync"},                                  /* 同步 */
    {"\xE6\x8B\x96\xE5\x8A\xA8", "Drag"},                                  /* 拖动 */
    {"\xE5\xAE\x9E\xE6\x97\xB6", "Live"},                                  /* 实时 */
    {"\xE8\xAF\xA6\xE7\xBB\x86", "Verbose"},                               /* 详细 */
    {"\xE6\x99\xAE\xE9\x80\x9A", "Info"},                                  /* 普通 */

    /* --- the grid's "more" card --- */
    {"\xE6\x9B\xB4\xE5\xA4\x9A", "More"},                                  /* 更多 */

    /* --- video decode, as the user asked for it: TWO choices, not four --- */
    {"\xE7\xA1\xAC\xE8\xA7\xA3", "Hardware"},                              /* 硬解 */
    {"\xE8\xBD\xAF\xE8\xA7\xA3", "Software"},                              /* 软解 */

    /* --- the search screen --- */
    {"\xE6\x90\x9C\xE7\xB4\xA2", "Search"},                                /* 搜索 */
    {"\xE6\x8C\x89 A \xE8\xBE\x93\xE5\x85\xA5\xE5\x85\xB3\xE9\x94\xAE\xE8\xAF\x8D",
     "Press A to type a keyword"},                                         /* 按 A 输入关键词 */
    {"\xE8\xBE\x93\xE5\x85\xA5\xE5\x89\xA7\xE5\x90\x8D\xEF\xBC\x88\xE5\x8F\xAF\xE6\x8B\xBC"
     "\xE9\x9F\xB3\xEF\xBC\x89",
     "Type a title (pinyin works)"},                                       /* 输入剧名（可拼音） */
    {"\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x90\x9C\xE7\xB4\xA2\xE2\x80\xA6", "Searching..."}, /* 正在搜索… */
    {"\xE6\xB2\xA1\xE6\x9C\x89\xE6\x89\xBE\xE5\x88\xB0", "No results"},   /* 没有找到 */
    {"\xE6\x98\xBE\xE7\xA4\xBA %u \xE6\x9D\xA1", "%u results"},           /* 显示 %u 条 */
    {"\xEF\xBC\x88\xE5\x85\xB1 %lld \xE6\x9D\xA1\xEF\xBC\x89", " (%lld total)"}, /* （共 %lld 条） */
    {"\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x8A\xA0\xE8\xBD\xBD\xE2\x80\xA6", "Loading..."},  /* 正在加载… */
    /* 开源许可 -- the sixth section of the settings screen (C0). Its own tab lists the licenses of
     * everything this binary is built from; see makeLicenseGroup in settings_activity.cpp. */
    {"\xE5\xBC\x80\xE6\xBA\x90\xE8\xAE\xB8\xE5\x8F\xAF", "Licenses"},
    /* （上游 %u 条 + 本站 %u 条） */
    {"\xEF\xBC\x88\xE4\xB8\x8A\xE6\xB8\xB8 %u \xE6\x9D\xA1 + \xE6\x9C\xAC\xE7\xAB\x99 %u \xE6\x9D\xA1\xEF\xBC\x89",
     " (%u upstream + %u local)"},
    {"\xE9\x9F\xB3\xE9\x87\x8F", "Volume"},                                 /* 音量 */
    {"\xE5\x80\x8D\xE9\x80\x9F", "Speed"},                                  /* 倍速 */
    {"\xE7\xBC\xA9\xE6\x94\xBE", "Zoom"},                                   /* 缩放 */
    {"\xE5\x88\x86\xE8\xBE\xA8\xE7\x8E\x87", "Quality value"},               /* 分辨率（值） */
    {"\xE5\xB7\xA6\xE5\x8F\xB3\xE6\x8B\x96\xE5\x8A\xA8", "drag left/right"},  /* 左右拖动 */
    {"\xE7\x94\xBB\xE9\x9D\xA2\xE8\xAE\xBE\xE7\xBD\xAE", "Picture settings"},  /* 画面设置 */
    /* ---- THE TWO REFUSAL SENTENCES (build175) ----------------------------------------------------
     * See the long note in hg_lang.h for why these exist and, more importantly, for what they do NOT
     * claim. Shown in place of the diagnostic the console used to print ("...after 20 attempts over 3
     * quality tiers. The server said HTTP 403-..."), which is accurate and useless to a viewer.
     * The Chinese is a statement about the SERVICE's answer, not a guess at a business rule. */
    /* 该集需要服务端授权后才能观看 */
    {"\xE8\xAF\xA5\xE9\x9B\x86\xE9\x9C\x80\xE8\xA6\x81\xE6\x9C\x8D\xE5\x8A\xA1\xE7\xAB\xAF\xE6"
     "\x8E\x88\xE6\x9D\x83\xE5\x90\x8E\xE6\x89\x8D\xE8\x83\xBD\xE8\xA7\x82\xE7\x9C\x8B",
     "This episode needs the service's permission to play"},
    /* 该清晰度需要服务端授权 */
    {"\xE8\xAF\xA5\xE6\xB8\x85\xE6\x99\xB0\xE5\xBA\xA6\xE9\x9C\x80\xE8\xA6\x81\xE6\x9C\x8D\xE5"
     "\x8A\xA1\xE7\xAB\xAF\xE6\x8E\x88\xE6\x9D\x83",
     "That quality needs the service's permission"}
};

static int g_lang = HG_LANG_ZH;

static int strEqCI(const char *a, const char *b) {
  size_t i;
  if (!a || !b) return 0;
  for (i = 0; a[i] && b[i]; i++) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
  }
  return a[i] == '\0' && b[i] == '\0';
}

void hg_lang_set(int lang) {
  if (lang < 0 || lang >= HG_LANG_COUNT) lang = HG_LANG_ZH;
  g_lang = lang;
}

int hg_lang_get(void) { return g_lang; }

int hg_lang_parse(const char *value) {
  if (!value || value[0] == '\0') return HG_LANG_ZH;
  /* "FOLLOW THE SYSTEM" IS A THIRD CHOICE, NOT A LANGUAGE. The reference layout offers it first, and the
   * console already knows its own language (setsysGetSystemLanguage), so asking it beats asking the user to
   * repeat what the system says. This function only reports the CHOICE; hg_boot resolves it, because that is
   * the layer that can talk to set:sys -- and keeping the resolution out of here is what lets the host tests
   * cover these rules on a machine with no console services. */
  if (strEqCI(value, "auto") || strEqCI(value, "system") || strEqCI(value, "follow")) return HG_LANG_AUTO;
  if (strEqCI(value, "en") || strEqCI(value, "eng") || strEqCI(value, "english")) return HG_LANG_EN;
  if (strEqCI(value, "zh") || strEqCI(value, "cn") || strEqCI(value, "chinese") ||
      strEqCI(value, "zh-hans") || strEqCI(value, "zh_cn")) {
    return HG_LANG_ZH;
  }
  /* Anything else is the default, not an error: the cfg file is hand-edited and a typo should leave
   * a readable interface rather than an empty one. */
  return HG_LANG_ZH;
}

const char *hg_lang_name(int lang) {
  if (lang < 0 || lang >= HG_LANG_COUNT) lang = HG_LANG_ZH;
  /* The name of a language IN that language: someone looking for Chinese looks for a Chinese word,
   * so the Chinese entry is used for Chinese whichever language is in force.
   *
   * The first version of this was `TABLE[HG_T_LANGUAGE_ZH + (lang == EN ? 1 : 0)].en`, which reads
   * the English COLUMN of the Chinese row and therefore answered "Chinese" for the Chinese language
   * -- wrong, and the host suite caught it ("Chinese is named in Chinese"). Ask for the row's own
   * column, which is the whole point of naming a language in itself. */
  if (lang == HG_LANG_EN) return TABLE[HG_T_LANGUAGE_EN].zh;   /* "English", the same in both */
  return TABLE[HG_T_LANGUAGE_ZH].zh;                           /* the Chinese word for Chinese */
}

const char *hg_tr(int key) {
  if (key < 0 || key >= HG_T_COUNT) return "";
  if (g_lang == HG_LANG_ZH) {
    const char *zh = TABLE[key].zh;
    if (zh && zh[0] != '\0') return zh;
  }
  return TABLE[key].en ? TABLE[key].en : "";
}
