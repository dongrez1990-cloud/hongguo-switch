/*
 * Settings screen implementation. See settings_activity.hpp.
 *
 * GPL-3.0-only.
 *
 * ASCII-only source file.
 */

#include "settings_activity.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hg_cfg.hpp"
#include "hg_client.hpp"
#include "hg_hints.hpp"
#include "hg_log.hpp"
#include "hg_thread.hpp"
#include "hg_selfcheck.hpp"   /* the play-chain check this screen can run on demand (build157) */

#include <borealis/views/cells/cell_input.hpp>

extern "C" {
#include "hg_httpcfg.h"
#include "hg_lang.h"
}

/* libnx, for the radio states and the IP/DNS configuration. Included through
 * <switch.h> so the struct layouts come from the SDK rather than from a guess. */
#include <switch.h>

namespace hg {
namespace {

/* ---- the table ---------------------------------------------------------- */

/* One settable row: a key in hongguo.cfg, its label, and the values it may take.
 * The FIRST option is the built-in default, which is what the app uses when the
 * key is absent from the file -- so an option list is also the statement of what
 * "unset" means.
 *
 * `kind` SAYS WHICH WIDGET AND WHICH VALIDATION. Everything was a selector until the
 * HTTP settings arrived, whose values are numbers and a URL: a selector cannot express
 * "any port between 1 and 600", and pretending otherwise (offering 10/20/30) would be a
 * menu that silently cannot set the value the user wants. So a row is either a SELECTOR
 * (a fixed list) or a NUMBER (free input, with a range) or TEXT (free input). */
enum RowKind {
    ROW_SELECTOR = 0,
    ROW_NUMBER,
    ROW_TEXT,
    /* A FACT, NOT A CHOICE. Used for a setting the app has only one working value for: the row is shown
     * so the user knows what is in force, and is not focusable so they cannot walk into a crash. */
    ROW_READONLY
};

struct Row {
    const char* key;
    const char* label;
    const char* options;   /* '/' separated; SELECTOR only */
    bool restart;          /* value is read once at startup, so a restart applies it */
    int kind;              /* RowKind */
    long min;              /* NUMBER: inclusive bounds */
    long max;
    const char* unit;      /* NUMBER/TEXT: shown as the hint */
};

/* A row's label, in the interface language.
 *
 * The row table carries an English label because that is what the log lines and the cfg comments use --
 * a key and its English name are the stable identity. But the SCREEN has to be readable in the language
 * the user chose, and the default is Chinese.
 *
 * A lookup table here rather than one hg_lang key inside each Row: these are the settings' own names,
 * reused nowhere else, and forty more entries in the shared enum would bloat the table the host suite
 * walks. The Chinese strings themselves live in hg_lang's table, so they are checked for
 * well-formedness like every other string.
 *
 * A key with no entry falls back to its English label -- a new setting shows up in English rather than
 * as a blank line. */
struct LabelKey { const char* key; int langKey; };

const LabelKey LABEL_KEYS[] = {
    {"renderer", HG_T_LB_RENDERER},        {"hwdec", HG_T_LB_HWDEC},
    {"ao", HG_T_LB_AO},                    {"overlay.labels", HG_T_LB_OVERLAY},
    {"mpv.msglevel", HG_T_LB_MSGLVL},      {"player.menu", HG_T_LB_PMENU},
    {"player.scrub", HG_T_LB_SCRUB},       {"player.quality", HG_T_LB_QUALITY},
    {"ui.scale", HG_T_LB_UISCALE},         {"home.tiers", HG_T_LB_TIERS},
    {"image.enable", HG_T_LB_IMAGES},      {"home.layout", HG_T_LB_HOMELAYOUT},
    {"home.cellW", HG_T_LB_CELLW},         {"detail.episodes", HG_T_LB_EPMODE},
    {"detail.cellW", HG_T_LB_EPCELLW},     {"home.refresh", HG_T_LB_REFRESH},
    {"language", HG_T_LB_LANGUAGE},        {"http.timeout", HG_T_LB_TIMEOUT},
    {"http.connect.timeout", HG_T_LB_CONNECT},
    {"http.dns.cache", HG_T_LB_DNSCACHE},  {"http.redirect", HG_T_LB_REDIRECT},
    {"http.tls.verify", HG_T_LB_TLS},      {"http.proxy", HG_T_LB_PROXY},
    {"image.threads", HG_T_LB_IMGTHREADS}, {"overlay", HG_T_LB_OVERLAYON},
    {"renderctx", HG_T_LB_RENDERCTX},      {"loadfile", HG_T_LB_LOADFILE},
    {"advanced", HG_T_LB_ADVANCED},        {"fencewait", HG_T_LB_FENCEWAIT},
    {"fetch", HG_T_LB_FETCH},              {"player.shotprobe", HG_T_LB_SHOTPROBE},
};

const char* rowLabel(const Row& row) {
    for (size_t i = 0; i < sizeof LABEL_KEYS / sizeof LABEL_KEYS[0]; i++) {
        if (std::strcmp(LABEL_KEYS[i].key, row.key) == 0) return hg_tr(LABEL_KEYS[i].langKey);
    }
    return row.label;
}

/* THE CHOICES TOO. A selector reading "auto/nvdec/nvdec-copy/no" is decipherable; "开/关" is the
 * difference between a screen a Chinese reader can use and one they must translate in their head.
 * Mapped by VALUE, so one word is translated once wherever it appears.
 *
 * `rowKey` IS PASSED BECAUSE ONE ROW NEEDS DIFFERENT WORDS FOR THE SAME VALUE. The decode row's "auto"
 * and "no" are the user choosing HARDWARE or SOFTWARE, not "on/off" -- but "no" on every other row does
 * mean 关. Translating by value alone forced every "no" in the screen to read 软解, which is why this
 * takes the key: the mapping is by value AND context, so the decode row says 硬解/软解 and a yes/no row
 * still says 开/关. */
const char* optionLabel(const std::string& rowKey, const std::string& opt) {
    if (rowKey == "hwdec") {
        /* mpv picks the Switch's NVDEC when it can; the value written to the file stays "auto"/"no" so
         * the cfg and everything that reads it are untouched. */
        if (opt == "auto") return hg_tr(HG_T_OPT_DEC_HW);
        if (opt == "no") return hg_tr(HG_T_OPT_DEC_SW);
        /* nvdec / nvdec-copy are no longer offered, but a cfg hand-edited to one of them still shows
         * something readable rather than the raw token. */
        if (opt == "nvdec") return "NVDEC";
        if (opt == "nvdec-copy") return "NVDEC-Copy";
    }
    if (opt == "yes") return hg_tr(HG_T_OPT_ON);
    if (opt == "no") return hg_tr(HG_T_OPT_OFF);
    if (opt == "auto") return hg_tr(HG_T_OPT_AUTO);
    if (opt == "grid") return hg_tr(HG_T_OPT_GRID);
    if (opt == "list") return hg_tr(HG_T_OPT_LIST);
    if (opt == "sw") return hg_tr(HG_T_OPT_SOFTWARE);
    if (opt == "deko3d") return hg_tr(HG_T_OPT_GPU);
    if (opt == "null") return hg_tr(HG_T_OPT_NONE);
    if (opt == "gpu") return hg_tr(HG_T_OPT_GPU);
    if (opt == "cpu") return hg_tr(HG_T_OPT_CPU);
    if (opt == "async") return hg_tr(HG_T_OPT_ASYNC);
    if (opt == "sync") return hg_tr(HG_T_OPT_SYNC);
    if (opt == "drag") return hg_tr(HG_T_OPT_DRAG);
    if (opt == "live") return hg_tr(HG_T_OPT_LIVE);
    if (opt == "verbose") return hg_tr(HG_T_OPT_VERBOSE);
    if (opt == "info") return hg_tr(HG_T_OPT_INFO);
    if (opt == "zh") return hg_tr(HG_T_LANGUAGE_ZH);
    /* "720p", "hos", numbers and the decode method names need no translation: nullptr means "show it
     * as it is". */
    return nullptr;
}

/* One tab's worth of rows.
 *
 * `titleKey` REPLACED THE LITERAL TITLE. The tab names were English strings, which meant the one
 * screen whose whole job is to be understood was the one screen the language setting could not
 * change. It is an hg_lang key now; the About tab passes -1 and keeps its literal, because "About"
 * is one of the words that reads the same either way and translating it adds nothing. */
struct Group {
    const char* title;     /* used when titleKey is negative */
    const Row* rows;
    size_t count;
    int titleKey;          /* an HG_T_* key, or -1 for `title` */
};

/* The group's heading in the language in force. */
const char* groupTitle(const Group& g) {
    if (g.titleKey >= 0) return hg_tr(g.titleKey);
    return g.title ? g.title : "";
}

/* Labels are in English like the rest of this app's own text; the CONTENT (titles,
 * tags) is upstream Chinese and is displayed as-is. A translated UI needs the
 * language setting and a label table, which is a separate piece of work. */

const Row PLAYBACK_ROWS[] = {
    /* THE RENDERER ROW HAS EXACTLY ONE WORKING CHOICE, and saying so is the point.
     *
     * MEASURED: switching this to `deko3d` on the console CRASHES THE APP. That is not a new discovery --
     * the project's own handover records that mpv's deko3d render API writes never reach the presented
     * framebuffer, and that the software path exists precisely because that went unsolved after three
     * independent readings plus a clean fencewait control. What IS new is that offering it as a menu item
     * lets a user walk into a crash to find that out.
     *
     * So `deko3d` is no longer offered here. The cfg can still be hand-edited to it by anyone who wants to
     * retry the investigation -- the value is still recognised -- but the screen does not invite a user
     * into a crash. Removing a choice that crashes is not hiding a feature; it is not shipping a trap. */
    {"renderer", "Renderer", "deko3d", false, ROW_READONLY, 0, 0, "GPU render path"},
    /* VIDEO DECODE, and this row is why the "not restart" flag matters.
     *
     * The value is read when mpv is set up, which happens once per player -- so a change applies to the
     * NEXT video opened, not to one already playing. Marking it as not needing a restart is therefore
     * literally true and is the more useful of the two settings: a user comparing "auto" against "no"
     * because a stream stutters does not want to relaunch the app between attempts.
     *
     * FOUR CHOICES, and the two extra ones exist for a specific failure. `auto` asks mpv to pick, and
     * mpv is allowed to fall back to software -- which for HEVC (this service's `bytevc1` streams) is a
     * plausible fallback on this hardware, and an unexplained one from the user's side. Naming `nvdec`
     * forces the Switch's hardware decoder, and `nvdec-copy` forces the variant that copies frames back
     * to system memory -- which is what a filter chain (the picture menu's brightness/contrast, say)
     * needs, because a direct hardware decoder bypasses filters. wiliwili switches to `auto-copy` for
     * exactly that reason when its mirror mode is on (mpv_core.cpp:1254-1261).
     *
     * The row shows which one is IN FORCE, not just which one was requested -- `hwdec-current` is read
     * after mpv starts and logged, because "requested" and "in force" are different facts. */
    {"hwdec", "Video decode", "auto/no", false, ROW_SELECTOR, 0, 0, nullptr},
    {"ao", "Audio output", "hos/null", false, ROW_SELECTOR, 0, 0, nullptr},
    {"overlay.labels", "Player overlay", "yes/no", false, ROW_SELECTOR, 0, 0, nullptr},
    {"mpv.msglevel", "mpv log level", "info/verbose", false, ROW_SELECTOR, 0, 0, nullptr},
    /* Added in round 13, when the six wiliwili-alignment steps landed. They are all read
     * at use time rather than at startup, except player.scrub which is read per frame. */
    {"player.menu", "Picture menu (X)", "yes/no", false, ROW_SELECTOR, 0, 0, nullptr},
    {"player.scrub", "Seek style", "drag/live", false, ROW_SELECTOR, 0, 0, nullptr},
    {"player.quality", "Quality row (X)", "yes/no", false, ROW_SELECTOR, 0, 0, nullptr},
    /* THE TWO DECODE COST SWITCHES (C1, C3). Both need a restart because they are mpv OPTIONS, applied before
     * the first file; the comment in setupMpv says why that is not a limitation this app can lift.
     *
     * `player.decode.fast` is the one setting here that makes the picture WORSE on purpose -- it exists for the
     * §2.4 stutter, and the row says "Low-quality" rather than "Smooth" so nobody turns it on by accident. */
    {"player.decode.fast", "Low-quality decode", "yes/no", true, ROW_SELECTOR, 0, 0, nullptr},
    {"player.cache.mb", "Decode cache", "0/32/64/128/256", true, ROW_SELECTOR, 0, 0, nullptr},
    /* THE SOFTWARE SURFACE HEIGHT (I2). See the note in setupMpv: this is the one setting that trades picture
     * sharpness for frame rate on the console's software path, so it is a choice rather than a default. 0 keeps
     * the window size (what every build before this one used). */
    {"player.sw.height", "Software surface", "0/540/720", true, ROW_SELECTOR, 0, 0, nullptr},
};

const Row APPEARANCE_ROWS[] = {
    {"ui.scale", "UI resolution", "720p/544p/900p/1080p", true, ROW_SELECTOR, 0, 0, nullptr},
    {"home.tiers", "Home sections", "yes/no", true, ROW_SELECTOR, 0, 0, nullptr},
    {"image.enable", "Cover images", "yes/no", false, ROW_SELECTOR, 0, 0, nullptr},
    {"home.layout", "Home layout", "grid/list", true, ROW_SELECTOR, 0, 0, nullptr},
    {"home.cellW", "Poster width", nullptr, false, ROW_NUMBER, 64, 600, "logical px"},
    {"detail.episodes", "Episode picker", "grid/list", true, ROW_SELECTOR, 0, 0, nullptr},
    {"detail.cellW", "Episode button width", nullptr, false, ROW_NUMBER, 64, 600, "logical px"},
    {"home.refresh", "Refresh button", "yes/no", false, ROW_SELECTOR, 0, 0, nullptr},
    /* THE LANGUAGE ROW. It sits on Appearance because its effect is on what the app SAYS rather
     * than on what it does, and it carries the restart flag like every other startup-read key --
     * the interface is built once per push, so a change applies on the next launch. */
    {"language", "Language", "auto/zh/en", true, ROW_SELECTOR, 0, 0, nullptr},
    /* THE THEME (C9) -- AND IT IS A RESTART ROW AFTER ALL, WHICH IS A CORRECTION RATHER THAN A PREFERENCE.
     *
     * The row was shipped as "applies immediately" on the strength of borealis re-recording its static command
     * list. The console disagreed: changing it CRASHED (Data Abort inside
     * SwitchVideoContext::recordStaticCommands, the console's own crash reports), and because the settings screen
     * had already written `theme=dark` to the cfg, every later launch crashed on startup as well -- the row
     * turned the app into something that would not start.
     *
     * The reason is visible in the framework once you look: setThemeVariant() re-records the deko3d command list
     * that the GPU may still be executing (switch_platform.cpp:293-299),
     * and it dereferences the video context unconditionally, which does not exist yet during boot. Neither is
     * something a row handler can make safe, so the honest row is the ordinary one: write the key, and say a
     * restart is needed. */
    {"theme", "Theme", "auto/dark/light", true, ROW_SELECTOR, 0, 0, nullptr},
    /* THE THREE DISPLAY/INPUT SWITCHES (C8, C12, C13). All three are applied at STARTUP (hg_boot), which is why
     * they carry the restart flag: borealis takes the frame limit before the first frame and the input swap
     * before the first key, so a mid-session change would be a claim the framework does not honour. */
    {"app.fps.limit", "Frame limit", "0/30/60/120", true, ROW_SELECTOR, 0, 0, nullptr},
    {"app.fps.show", "Show FPS", "yes/no", true, ROW_SELECTOR, 0, 0, nullptr},
    {"app.swap.ab", "Swap A/B and X/Y", "yes/no", true, ROW_SELECTOR, 0, 0, nullptr},
    /* THE HINT BAR (C11). NOT a restart row: the bar re-checks this key every frame (see hg_hints.cpp), so the
     * change is visible the moment it is made -- which is also what makes the row testable by the person using
     * it, rather than by the next launch. */
    {"app.hints", "Button hints", "yes/no", false, ROW_SELECTOR, 0, 0, nullptr},
};

/* THE HTTP GROUP, AND WHY IT REUSES THE PARSER RATHER THAN REPEATING THE RULES.
 *
 * hg_httpcfg already decides what is valid: timeout 1..600, a connect timeout no larger
 * than the total, a proxy that carries a scheme. Those rules were written for the startup
 * path, and a settings screen that accepted `0` here while startup refused it would be
 * two sources of truth for one fact -- the exact thing this project keeps paying to avoid.
 *
 * So the writers below call hg_httpcfg_apply on a scratch config first: if it refuses, the
 * screen shows the REASON and does NOT write the file. The bounds in the Row table are
 * duplicated from hg_httpcfg.h ON PURPOSE and are checked to agree by hg_httpcfg_test's
 * own ranges -- but the authoritative answer is the parser's.
 *
 * EVERY VALUE HERE NEEDS A RESTART, because Client::globalInit reads them once. The
 * screen says so on each row rather than leaving the user to wonder why nothing changed. */
const Row NETWORK_ROWS[] = {
    /* THE RUMBLE SWITCH (C15) LIVES HERE, under 其他设置, like the reference layout. Unlike the three
     * display switches above it applies IMMEDIATELY: hg_haptic reads the flag when it fires, and the act of
     * turning it on gives one pulse so the user can feel that it works -- a vibration setting with no feedback
     * is indistinguishable from a broken one. */
    {"app.rumble", "Vibration", "yes/no", false, ROW_SELECTOR, 0, 0, nullptr},
    {"http.timeout", "Request timeout", nullptr, true, ROW_NUMBER, 1, 600, "seconds"},
    {"http.connect.timeout", "Connect timeout", nullptr, true, ROW_NUMBER, 1, 600, "seconds"},
    {"http.dns.cache", "DNS cache", nullptr, true, ROW_NUMBER, 0, 3600, "seconds"},
    {"http.redirect", "Follow redirects", "yes/no", true, ROW_SELECTOR, 0, 0, nullptr},
    {"http.tls.verify", "Verify certificate", "yes/no", true, ROW_SELECTOR, 0, 0, nullptr},
    {"http.proxy", "Proxy", nullptr, true, ROW_TEXT, 0, 0, "http://host:port"},
    {"image.threads", "Image fetches at once", nullptr, false, ROW_NUMBER, 1, 16, "1-16"},
};

/* The diagnostic group is not filler. Every open question in this project has been
 * answered by flipping one of these and reading the log, and each flip used to cost
 * a rebuild, a 26 MB upload and a walk to the console. */
const Row DIAGNOSTIC_ROWS[] = {
    {"overlay", "Draw the UI", "yes/no", true, ROW_SELECTOR, 0, 0, nullptr},
    {"renderctx", "Create render context", "yes/no", true, ROW_SELECTOR, 0, 0, nullptr},
    {"loadfile", "Open the stream", "yes/no", true, ROW_SELECTOR, 0, 0, nullptr},
    {"advanced", "Advanced control", "yes/no", true, ROW_SELECTOR, 0, 0, nullptr},
    {"fencewait", "Fence wait", "gpu/cpu", true, ROW_SELECTOR, 0, 0, nullptr},
    {"fetch", "Home fetch", "async/sync", true, ROW_SELECTOR, 0, 0, nullptr},
    {"player.shotprobe", "Screenshot probe", "yes/no", true, ROW_SELECTOR, 0, 0, nullptr},
};

const Group GROUPS[] = {
    {nullptr, PLAYBACK_ROWS, sizeof PLAYBACK_ROWS / sizeof PLAYBACK_ROWS[0], HG_T_SETTINGS_PLAYBACK},
    {nullptr, APPEARANCE_ROWS, sizeof APPEARANCE_ROWS / sizeof APPEARANCE_ROWS[0], HG_T_SETTINGS_APPEARANCE},
    {nullptr, NETWORK_ROWS, sizeof NETWORK_ROWS / sizeof NETWORK_ROWS[0], HG_T_SETTINGS_NETWORK},
    {nullptr, DIAGNOSTIC_ROWS, sizeof DIAGNOSTIC_ROWS / sizeof DIAGNOSTIC_ROWS[0], HG_T_SETTINGS_DIAGNOSTICS},
};

/* The mpv log level is a message-level STRING, not one of our words: selecting
 * "verbose" has to write the real mpv syntax or mpv would reject it. Mapping it
 * here rather than in the row table keeps the table readable. */
/* THE ONE SETTING THIS SCREEN APPLIES WITHOUT A RESTART. "auto" hands the decision back to the platform (which
 * already asked the console), and dark/light force it; both are one call, and both are logged with what the
 * platform reports afterwards -- a theme that did not change looks exactly like a theme that was not asked to. */


std::string valueToWrite(const Row& row, const std::string& option) {
    if (std::strcmp(row.key, "mpv.msglevel") == 0) {
        if (option == "verbose") return "all=info,cplayer=info,vo=v,libmpv=v,mpv=v";
        return "info";
    }
    return option;
}

/* ---- writing a value, with the parser as the authority ---------------------
 *
 * A NUMBER row is validated by hg_httpcfg_apply on a scratch config before anything is
 * written. The reasons it can give -- timeout 0 would mean "never", a connect timeout above
 * the total can never be reached, a proxy needs a scheme -- are exactly the ones the startup
 * path logs, and the screen shows the same sentence. That is the point: one set of rules,
 * two places they are enforced, and no way for the screen to accept what startup refuses. */
bool writeNumberRow(const Row* row, long value, std::string* problem) {
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", value);

    /* Only the keys hg_httpcfg knows are run through it; the two layout widths are ours and
     * have no parser, so they use the bounds in the Row table. */
    if (strncmp(row->key, "http.", 5) == 0) {
        HgHttpCfg scratch;
        const char* why = nullptr;
        hg_httpcfg_defaults(&scratch);
        /* The nested rule needs the CURRENT total, so it is loaded first -- the same
         * order-dependence the config file has, and for the same reason. */
        hg_httpcfg_apply(&scratch, "http.timeout", cfgValue("http.timeout", "20").c_str(), nullptr);
        int rc = hg_httpcfg_apply(&scratch, row->key, buf, &why);
        if (rc == -1) {
            *problem = why ? why : "refused by the HTTP configuration rules";
            return false;
        }
    } else {
        if (value < row->min || value > row->max) {
            char b[128];
            snprintf(b, sizeof b, "must be between %ld and %ld", row->min, row->max);
            *problem = b;
            return false;
        }
    }

    if (!hg::cfgWrite(row->key, buf)) {
        *problem = "could not be written to hongguo.cfg -- see the log";
        return false;
    }
    return true;
}

/* A TEXT row, validated the same way. An EMPTY value is refused rather than written: the
 * file's convention is that an absent key means "default", so writing `http.proxy=` would be
 * a line that looks like a setting and means nothing -- and hg_httpcfg refuses it for exactly
 * that reason at startup, so the screen must not create the state startup complains about. */
bool writeTextRow(const Row* row, const std::string& value, std::string* problem) {
    if (value.empty()) {
        *problem = "empty; use the selector/revert instead of clearing it";
        return false;
    }
    if (strncmp(row->key, "http.", 5) == 0) {
        HgHttpCfg scratch;
        const char* why = nullptr;
        hg_httpcfg_defaults(&scratch);
        int rc = hg_httpcfg_apply(&scratch, row->key, value.c_str(), &why);
        if (rc == -1) {
            *problem = why ? why : "refused by the HTTP configuration rules";
            return false;
        }
    }
    if (!hg::cfgWrite(row->key, value.c_str())) {
        *problem = "could not be written to hongguo.cfg -- see the log";
        return false;
    }
    return true;
}

/* Which option is in force, and what to write for it. Returns the index; when the
 * file holds a value that is not in the list -- a hand-edited cfg, or a value left
 * over from an older build -- index 0 is returned so the screen shows something
 * real rather than an empty selector. */
int currentIndex(const Row& row, const std::vector<std::string>& opts, bool* offList) {
    std::string cur = cfgValue(row.key, opts.empty() ? "" : opts[0].c_str());
    if (offList) *offList = false;
    for (size_t i = 0; i < opts.size(); i++) {
        if (valueToWrite(row, opts[i]) == cur) return (int)i;
    }
    if (offList) *offList = true;
    return 0;
}

std::vector<std::string> splitOptions(const char* options) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = options; *p; p++) {
        if (*p == '/') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/* ---- widgets ------------------------------------------------------------ */

brls::Label* makeHeader(const char* text) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(24.0f);
    label->setMarginTop(12.0f);
    label->setMarginBottom(6.0f);
    return label;
}

/* ONE ROW, DISPATCHED BY KIND.
 *
 * A SELECTOR keeps the original behaviour exactly. A NUMBER or TEXT row gets an input cell,
 * and its callback goes through the writers above, which consult the same parser the startup
 * path uses -- so the screen cannot accept a value the app would refuse to honour. */
void addRow(brls::Box* box, const Row* row) {
    if (row->kind == ROW_READONLY) {
        /* Shown, not offered. See ROW_READONLY. */
        auto* cell = new brls::DetailCell();
        cell->setText(rowLabel(*row));
        cell->setDetailText(row->unit ? row->unit : "");
        cell->setFocusable(false);
        box->addView(cell);
        return;
    }

    if (row->kind == ROW_NUMBER) {
        long current = (long)strtol(hg::cfgValue(row->key, "0").c_str(), nullptr, 10);
        /* A key that is absent shows the value the app would actually use. Without this the
         * row would read 0, which for http.timeout is precisely the value the parser refuses --
         * a screen offering to save something it will reject. */
        {
            std::string raw = hg::cfgValue(row->key, "");
            if (raw.empty()) {
                if (strcmp(row->key, "http.timeout") == 0) current = HG_HTTPCFG_TIMEOUT_DEFAULT;
                else if (strcmp(row->key, "http.connect.timeout") == 0) current = HG_HTTPCFG_CONNECT_DEFAULT;
                else if (strcmp(row->key, "http.dns.cache") == 0) current = HG_HTTPCFG_DNS_CACHE_DEFAULT;
                else if (strcmp(row->key, "image.threads") == 0) current = HG_HTTPCFG_IMG_THREADS_DEFAULT;
                else current = row->min;
            }
        }

        auto* cell = new brls::InputNumericCell();
        cell->init(rowLabel(*row), current,
                   [row](long value) {
                       std::string problem;
                       if (!writeNumberRow(row, value, &problem)) {
                           brls::Application::notify(std::string(row->label) + ": " + problem);
                           hg::logPrintf("SETTINGS: %s=%ld REFUSED -- %s", row->key, value,
                                         problem.c_str());
                           return;
                       }
                       hg::logPrintf("SETTINGS: %s = %ld%s", row->key, value,
                                     row->restart ? "  [needs restart]" : "");
                       if (row->restart) {
                           brls::Application::notify(std::string(row->label) +
                                                     ": written, applies after a restart");
                       }
                   },
                   row->unit ? row->unit : "");
        box->addView(cell);
        return;
    }

    if (row->kind == ROW_TEXT) {
        std::string current = hg::cfgValue(row->key, "");
        auto* cell = new brls::InputCell();
        cell->init(rowLabel(*row), current,
                   [row](std::string value) {
                       std::string problem;
                       if (!writeTextRow(row, value, &problem)) {
                           brls::Application::notify(std::string(row->label) + ": " + problem);
                           hg::logPrintf("SETTINGS: %s='%s' REFUSED -- %s", row->key, value.c_str(),
                                         problem.c_str());
                           return;
                       }
                       hg::logPrintf("SETTINGS: %s = '%s'%s", row->key, value.c_str(),
                                     row->restart ? "  [needs restart]" : "");
                       if (row->restart) {
                           brls::Application::notify(std::string(row->label) +
                                                     ": written, applies after a restart");
                       }
                   },
                   row->unit ? row->unit : "");
        box->addView(cell);
        return;
    }

    std::vector<std::string> opts = splitOptions(row->options);
    bool offList = false;
    int cur = currentIndex(*row, opts, &offList);

    auto* cell = new brls::SelectorCell();
    /* THE LABEL AND THE CHOICES ARE SHOWN IN THE INTERFACE LANGUAGE; the VALUE written to the file stays
     * the raw token, because that is what the app parses. Only the display is translated -- translating
     * what gets stored would break every cfg-reader in the project. */
    std::vector<std::string> shown;
    shown.reserve(opts.size());
    for (size_t i = 0; i < opts.size(); i++) {
        const char* tr = optionLabel(row->key, opts[i]);
        shown.push_back(tr ? tr : opts[i]);
    }
    /* The options and the row pointer are captured BY VALUE, and that is load-bearing: `row` is a local
     * `const Row*` PARAMETER, so capturing it by reference would capture the reference to a stack variable
     * that goes out of scope the moment this function returns -- and this callback only runs later, when
     * the user actually changes the value.
     *
     * THIS BUG WAS INTRODUCED ONCE ALREADY, while wiring up the Chinese labels. It is the exact symptom
     * the user reported: the settings screen OPENS fine and CRASHES on the first change, because the
     * callback is the only thing that touches the dangling pointer. The crash is also intermittent, which
     * fits -- a reference to a dead stack slot often still reads plausible garbage.
     *
     * The tables are static, so a captured Row* stays valid for the life of the process. */
    cell->init(rowLabel(*row), shown, cur, [row, opts, offList](int index) {
        if (index < 0 || index >= (int)opts.size()) return false;
        std::string want = valueToWrite(*row, opts[(size_t)index]);
        std::string before = cfgValue(row->key, opts[0].c_str());
        if (before == want) return true;   /* nothing to do, and nothing to write */

        if (!hg::cfgWrite(row->key, want.c_str())) {
            brls::Application::notify(std::string("Could not write ") + row->key +
                                      " to hongguo.cfg -- see the log");
            return false;                  /* refuse, and leave the cell unchanged */
        }
        if (row->restart) {
            brls::Application::notify(std::string(row->label) +
                                      ": written, applies after a restart");
        }
        hg::logPrintf("SETTINGS: %s = %s (was '%s')%s", row->key, want.c_str(),
                      before.c_str(), row->restart ? "  [needs restart]" : "");
        /* NOTHING IS APPLIED LIVE ANY MORE, AND THE THEME IS WHY. This used to call applyThemeNow() for the theme
         * row, on the strength of borealis re-recording its static command list. On the console that crashed --
         * see the theme row's comment above -- so the call is gone and the key is read at boot like every other
         * startup setting. (The one row that IS live, Button hints, needs nothing here: the hint bar re-reads its
         * own key every frame, and that is a plain cfg read rather than a framework re-record.) */
        return true;
    });
    box->addView(cell);

    if (offList) {
        /* Say so rather than silently showing option 0 as if it were in force. */
        auto* note = new brls::Label();
        note->setFontSize(14.0f);
        note->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
        char b[160];
        snprintf(b, sizeof b, "hongguo.cfg holds '%s=...' with a value not in this list",
                 row->key);
        note->setText(b);
        box->addView(note);
    }
}

brls::View* makeSettingGroup(const Group& g) {
    auto* scroll = new brls::ScrollingFrame();
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(16.0f, 24.0f, 24.0f, 24.0f);
    box->addView(makeHeader(groupTitle(g)));

    for (size_t i = 0; i < g.count; i++) {
        addRow(box, &g.rows[i]);
    }

    scroll->setContentView(box);
    return scroll;
}

/* A read-only line: title on the left, value on the right. */
brls::RecyclerCell* makeInfo(const std::string& title, const std::string& detail) {
    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(detail);
    return cell;
}

/* [[maybe_unused]] RATHER THAN DELETED: the tab that called this was removed at the user's request, and
 * restoring it is one line. Deleting the builder would mean rebuilding the whole page (build time, paths and the
 * key list) to get it back. The attribute is what keeps the zero-warning rule honest about that choice. */
[[maybe_unused]] brls::View* makeAboutGroup() {
    auto* scroll = new brls::ScrollingFrame();
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(16.0f, 24.0f, 24.0f, 24.0f);

    box->addView(makeHeader("This build"));

    /* THE FACTS A USER CANNOT GUESS, AND THE ONES THAT WENT STALE.
     *
     * The build stamp is the same __DATE__/__TIME__ pair the log header carries, so "which build is this" is
     * answerable from the screen as well as from hongguo.log -- see docs/构建号与产物对照.md.
     *
     * Two rows here USED TO BE WRONG and were fixed in this round, because a settings screen that lies is worse
     * than one that says nothing:
     *   - "Search: the endpoint answers 200 with 0 bytes" described the signed API, which is indeed dead. The
     *     SEARCH SCREEN does not use it any more: it reads the site's own search page (build38) and a local
     *     index (build42), and it works.
     *   - "A on the home header -> refresh" stopped being true when that row was removed and refresh moved to
     *     LB (build40).
     */
    char build[64];
    snprintf(build, sizeof build, "%s %s", __DATE__, __TIME__);
    box->addView(makeInfo("Build", build));
    box->addView(makeInfo("Settings file", "sdmc:/switch/hongguo.cfg"));
    box->addView(makeInfo("Watch history", "sdmc:/switch/hongguo-history.txt"));
    box->addView(makeInfo("Your list", "sdmc:/switch/hongguo-fav.txt"));
    box->addView(makeInfo("Config read at startup", "yes -- the About tab states the arm"));
    /* The list grew: the HTTP settings and the layout switches are all read once, so a row
     * marked "needs restart" here is a row that really does. Saying less than the truth was
     * already a small inaccuracy before the Network tab existed. */
    /* BOTH LISTS WERE OUT OF DATE, AND HERE IT IS NOT COSMETIC: a user reads these two lines to decide whether a
     * change needs a restart, so an incomplete list makes them guess. Round 29 added the rows this batch
     * introduced, split the way the code actually behaves:
     *
     *   - `theme` and `Button hints` apply AT ONCE (borealis re-records the static deko3d command list for the
     *     first; the hint bar re-reads its key every frame for the second);
     *   - the rest are read before the first frame or before the first stream, so they really do need a restart. */
    box->addView(makeInfo("Restart needed for",
                          "UI resolution, Home layout, Episode picker, Network, Diagnostics, Language, "
                          "Frame limit, Show FPS, Swap A/B, Low-quality decode, Decode cache, Software surface"));
    box->addView(makeInfo("Applied immediately",
                          "Theme, Button hints, Video decode (next video), player menu, seek style, quality "
                          "row, refresh"));
    box->addView(makeInfo("Cover fetch cap", "256 KiB per thumbnail, one retry at 1.5 MiB"));
    box->addView(makeInfo("Image fetches at once", "6 by default; see image.threads"));

    box->addView(makeHeader("Controls added for wiliwili parity"));
    box->addView(makeInfo("X in the player", "picture menu (brightness..quality)"));
    box->addView(makeInfo("Left/Right in the player", "drag the progress bar; release to seek"));
    box->addView(makeInfo("X on the home screen", "small / large cells"));
    box->addView(makeInfo("LB on the home screen", "refresh (the header row was removed)"));

    box->addView(makeHeader("Not provided, and why"));

    /* MEASURED, not assumed. Stating this on screen is the point: an absent feature
     * with no explanation reads as an oversight. */
    box->addView(makeInfo("Danmaku",
                          "the service sends none: 'danmaku'/'comment' occur 0 times"));
    box->addView(makeInfo("Comments",
                          "the service sends none: 'reply'/'barrage' occur 0 times"));
    box->addView(makeInfo("Live streams", "this service has no live rooms"));
    box->addView(makeInfo("Window on top / shortcuts / update",
                          "desktop concepts; no counterpart on Switch"));
    /* The two gaps that a user would otherwise report as bugs, with the measurement behind each. */
    box->addView(makeInfo("Thumbnail on the seek bar",
                          "needs a captured frame; see the shot probe in Diagnostics"));
    box->addView(makeInfo("Texture cache setting",
                          "nothing to cache: covers are loaded once per cell, never recycled"));
    box->addView(makeInfo("Search, and what it can return",
                          "works: the site's own search page, plus a local index. Upstream hands over ten "
                          "results and no way to ask for the next ten (fourteen parameter spellings tried)"));

    box->addView(makeHeader("Console controls"));
    box->addView(makeInfo("A / B", "open / back"));
    box->addView(makeInfo("Y (home screen)", "search -- the settings are reached from the bar"));
    box->addView(makeInfo("Y (detail screen)", "add / remove from your list"));
    box->addView(makeInfo("LB (home screen)", "refresh"));
    box->addView(makeInfo("A / X / Y (player)", "controls / picture menu / overlay text"));
    /* THE CONTROL BAR'S OWN KEYS, which the reference layout assumes and a user cannot guess: A brings the bar up,
     * UP/DOWN move between its items, and LEFT/RIGHT adjust the selected one -- or seek, when the PROGRESS item is
     * selected, which is why that item is the default. */
    box->addView(makeInfo("In the player's control bar", "UP/DOWN pick an item, LEFT/RIGHT change it "
                                                         "(on PROGRESS: seek)"));
    box->addView(makeInfo("ZL / ZR (player)", "previous / next episode"));
    /* ---- + DOES NOT QUIT ANY MORE (build167), AND SAYING SO HERE IS THE POINT -------------------------
     *
     * This line read "+ quit the application" until build167, and it was accurate: borealis' exit action
     * was registered on + for every screen. That action crashed the console (a cover fetch in flight
     * while exit() tore down libnx's socket service -- the two crash reports and their stacks are in
     * hg_boot.cpp, on setGlobalQuit(false)), so it is GONE from this build. The user's instruction:
     * the console's own Home menu closes the title, and + means the settings screen on the home page.
     *
     * A help table that still advertises a removed button is worse than no help table: it is the first
     * thing someone reads when they cannot find a way out. */
    box->addView(makeInfo("+ (home screen)", "open this settings screen"));
    box->addView(makeInfo("To close the app", "use the console's HOME button -> X -> close"));
    box->addView(makeInfo("Quit from inside the app", "removed on purpose -- it crashed the console"));

    scroll->setContentView(box);
    return scroll;
}

/* ---- the licenses tab (C23) ----------------------------------------------
 *
 * WHY THIS IS A REAL TAB AND NOT DECORATION. This binary is built from other people's work, and two of the
 * components are GPL: borealis (the UI framework, and the wiliwili fork of it this app's Switch bits came from)
 * and mpv's libmpv. Distributing a GPL binary without the notices is a licence problem, not a style question --
 * and it is also the honest place to say which parts are NOT ours.
 *
 * The list is what the build actually links, checked against the makefile and the deps rather than copied from
 * habit: borealis/wiliwili, libmpv, libnx, deko3d, nanovg, and the C parts of this app itself. */
/* See the note on makeAboutGroup. This one also carries the GPL/LGPL notices, and those are preserved in
 * docs/第三方组件与许可.md precisely so that removing the tab cannot lose them. */
[[maybe_unused]] brls::View* makeLicenseGroup() {
    auto* scroll = new brls::ScrollingFrame();
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(16.0f, 24.0f, 24.0f, 24.0f);

    box->addView(makeHeader("This application"));
    box->addView(makeInfo("hongguo-switch", "GPL-3.0-only. A client for hongguoduanju.com, written for the "
                                            "Nintendo Switch."));

    box->addView(makeHeader("Built from"));
    box->addView(makeInfo("borealis + wiliwili", "GPL-3.0. The UI framework, and the Switch fork this app's "
                                                 "grid, player and platform code follow."));
    box->addView(makeInfo("libmpv (mpv)", "LGPL-2.1-or-later / GPL-2.0-or-later depending on the build; this "
                                          "app links the shared library and changes nothing in it."));
    box->addView(makeInfo("libnx", "ISC. The Switch homebrew runtime: services, threads, the software keyboard."));
    box->addView(makeInfo("deko3d", "MIT / zlib-style. The GPU API the frames are presented through."));
    box->addView(makeInfo("nanovg", "zlib. The 2D drawing this app's overlays are made of."));
    box->addView(makeInfo("nlohmann/json", "MIT. Used by the host-side test fixtures."));

    box->addView(makeHeader("Not included"));
    /* SAYING WHAT IS ABSENT IS PART OF A LICENCE PAGE. A reader who expects the upstream brand assets or a
     * bundled font should find out here rather than by looking. */
    box->addView(makeInfo("Brand assets", "none: no upstream logo or artwork is redistributed"));
    box->addView(makeInfo("Fonts", "none bundled; the console provides the system font"));
    box->addView(makeInfo("Source", "the notices above name the projects; their own licence files ship with "
                                    "the devkitPro packages this app is built against"));

    scroll->setContentView(box);
    return scroll;
}

/* ---- the network diagnostic tab ------------------------------------------
 *
 * PORTED FROM wiliwili's fragment/setting_network.cpp, and it is worth saying what was
 * kept and what was not. KEPT: the shape -- radio states, IP, DNS, an API test, and a
 * clock readout. NOT KEPT: its network-time request, because that calls Bilibili's
 * get_unix_time endpoint and this service was never observed to offer one. The
 * server's own HTTP Date header takes its place, which needs no invented endpoint and
 * measures the same thing (the clock skew the signed requests are exposed to).
 *
 * WHY THIS IS WORTH A TAB AT ALL. Every problem this project has had was diagnosed
 * from the breadcrumb log over FTP. Seeing connectivity, the IP, the DNS servers,
 * whether the API answers, and whether the console clock is wrong -- on the screen,
 * with no PC -- turns the most common class of "it did not work" into something the
 * user can answer themselves.
 *
 * IT IS HONEST ABOUT WHAT IT CANNOT CHECK. There is no DNS-latency probe and no
 * traceroute here: both would need a second transport (raw sockets) and would be a
 * guess about what a failure means. What is shown is what was measured. */

enum NetRowId {
    NET_ROW_WIFI = 0,
    NET_ROW_IP,
    NET_ROW_DNS,
    NET_ROW_API,
    NET_ROW_HTTP,
    NET_ROW_BODY,
    NET_ROW_ROUTER,
    NET_ROW_CARDS,
    NET_ROW_ELAPSED,
    NET_ROW_SYS_TIME,
    NET_ROW_NET_TIME,
    NET_ROW_SKEW,
    NET_ROW_COUNT
};

struct NetState {
    brls::DetailCell* rows[NET_ROW_COUNT] = {};
    brls::Label* note = nullptr;
};

/* The rows, in order, with their titles and their pre-probe text. One table, so the
 * row count and the labels cannot drift apart. */
struct NetRowDef {
    const char* title;
    const char* initial;
};
const NetRowDef NET_ROWS[NET_ROW_COUNT] = {
    {"WiFi", "checking..."},
    {"IP address", "-"},
    {"DNS servers", "-"},
    {"API test", "not run yet"},
    {"HTTP status", "-"},
    {"Body size", "-"},
    {"_ROUTER_DATA", "-"},
    {"Cards parsed", "-"},
    {"Elapsed", "-"},
    {"Console clock", "-"},
    {"Server clock", "-"},
    {"Clock skew", "-"},
};

std::string hexIpv4(u32 addr) {
    /* libnx stores the four octets in the low bytes of the u32, first octet first, so
     * they are shifted out rather than the value being byte-swapped as a whole.
     * Getting this backwards prints a plausible but wrong address. */
    char b[24];
    snprintf(b, sizeof b, "%u.%u.%u.%u", (unsigned)(addr & 0xFF), (unsigned)((addr >> 8) & 0xFF),
             (unsigned)((addr >> 16) & 0xFF), (unsigned)((addr >> 24) & 0xFF));
    return std::string(b);
}

std::string hmsNow() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[32];
    snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(b);
}

/* Read the radio states and the IP/DNS configuration.
 *
 * nifmGetCurrentIpConfigInfo is used rather than a resolver lookup: it reports the
 * console's own CONFIGURED DNS servers, which is what "is the DNS wrong" actually
 * asks. The session is opened and closed here because this runs once, when the tab is
 * built, and nothing needs it afterwards. */
void fillNetworkBasics(NetState* st) {
    /* libnx's `Result` is a typedef for u32, and this file also includes hg_client.hpp
     * which declares `struct hg::Result` -- so an unqualified `Result` in this namespace
     * resolves to hg's, and the libnx calls below would not compile ("no match for
     * operator!=" against hg::Result). Every status variable here is therefore named
     * `nifmRc` and typed u32 explicitly, which is both unambiguous and what the libnx
     * prototypes actually return. */
    u32 nifmRc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(nifmRc)) {
        char b[96];
        snprintf(b, sizeof b, "nifm unavailable (rc=0x%08X)", (unsigned)nifmRc);
        st->rows[NET_ROW_WIFI]->setDetailText(b);
        st->rows[NET_ROW_IP]->setDetailText(b);
        st->rows[NET_ROW_DNS]->setDetailText(b);
        hg::logPrintf("NET: nifmInitialize failed rc=0x%08X", (unsigned)nifmRc);
        return;
    }

    /* The INTERNET connection status, not just the radio: "WiFi is on" and "the console
     * has a route to the internet" are different facts, and the second is the one that
     * matters to this app. */
    NifmInternetConnectionType type = (NifmInternetConnectionType)0;
    u32 wifiStrength = 0;
    NifmInternetConnectionStatus status = (NifmInternetConnectionStatus)0;
    u32 rcStatus = nifmGetInternetConnectionStatus(&type, &wifiStrength, &status);

    if (R_SUCCEEDED(rcStatus) && status == NifmInternetConnectionStatus_Connected) {
        const char* kind = (type == NifmInternetConnectionType_WiFi) ? "WiFi" : "Ethernet";
        char b[96];
        snprintf(b, sizeof b, "connected (%s, signal %u)", kind, (unsigned)wifiStrength);
        st->rows[NET_ROW_WIFI]->setDetailText(b);
    } else {
        char b[96];
        snprintf(b, sizeof b, "NOT connected (status=%d rc=0x%08X)", (int)status,
                 (unsigned)rcStatus);
        st->rows[NET_ROW_WIFI]->setDetailText(b);
    }

    /* The five out-parameters ARE the API: there is no config struct in this libnx
     * version. Checked in the header rather than assumed -- the first version of this
     * function declared a NifmCurrentIpConfigInfo, which does not exist. */
    u32 ip = 0, subnet = 0, gateway = 0, dns1 = 0, dns2 = 0;
    u32 rcIp = nifmGetCurrentIpConfigInfo(&ip, &subnet, &gateway, &dns1, &dns2);
    if (R_SUCCEEDED(rcIp)) {
        st->rows[NET_ROW_IP]->setDetailText(hexIpv4(ip));
        char b[96];
        snprintf(b, sizeof b, "%s / %s", hexIpv4(dns1).c_str(), hexIpv4(dns2).c_str());
        st->rows[NET_ROW_DNS]->setDetailText(b);
        hg::logPrintf("NET: ip=%s mask=%s gw=%s dns=%s/%s", hexIpv4(ip).c_str(),
                      hexIpv4(subnet).c_str(), hexIpv4(gateway).c_str(), hexIpv4(dns1).c_str(),
                      hexIpv4(dns2).c_str());
    } else {
        char b[96];
        snprintf(b, sizeof b, "unavailable (rc=0x%08X)", (unsigned)rcIp);
        st->rows[NET_ROW_IP]->setDetailText(b);
        st->rows[NET_ROW_DNS]->setDetailText(b);
        hg::logPrintf("NET: nifmGetCurrentIpConfigInfo failed rc=0x%08X", (unsigned)rcIp);
    }

    nifmExit();
}

/* Run the probe and put every measurement on its own row.
 *
 * ON A WORKER THREAD AND THEN brls::sync, because Client::networkProbe blocks: it does
 * a full HTTPS request. Running it on the main thread would freeze the UI for the
 * duration, which is exactly what this project's fetch paths already avoid. The thread
 * comes from hg_thread (libnx threadCreate), not std::thread, whose constructor throws
 * ENOSYS on this toolchain. */
void runProbe(NetState* st, const AliveToken& alive) {
    st->rows[NET_ROW_API]->setDetailText("running...");
    if (st->note) st->note->setText("Probe in flight (one HTTPS request to the home page).");

    /* ---- THE ROWS ARE LOCKED, BECAUSE THE TAB CAN GO AWAY WHILE THE PROBE RUNS --------------------------
     *
     * MEASURED ON HARDWARE (the console's crash report, 2026-10-10 14:40:02):
     *
     *     Result 0x4A8 / Data Abort
     *     brls::DetailCell::setDetailText()      cells/cell_detail.cpp:73
     *       <- brls::BoundView<brls::Label>::resolve()   (the detail label behind the cell)
     *       <- a task on the main thread
     *
     * and the user's own description of the trigger: "scrolling to 关于更多 / 开源许可 in the settings screen
     * crashes". SWITCHING TABS IS THE TRIGGER: borealis' TabFrame destroys the views of the tab it leaves, so the
     * network probe's callback was writing into cells that had already been freed -- while the ACTIVITY was
     * still alive, which is exactly why the liveness token from build74 did not catch it. The token answers "is
     * the screen still there", and the answer was yes.
     *
     * `ptrLock` is borealis' own answer to this, and the app already uses it for the cover loader: the deletion
     * pool refuses to delete a locked view, and the lock is released in the callback. Locking the cells keeps
     * them -- and the bound labels inside them -- valid until this callback has run, whatever happens to the tab
     * in between. The rows are simply detached from the tree if the user moved on, so writing to them is
     * harmless, and the pool frees them once the last lock is released.
     *
     * THE UNLOCKS COME FIRST, BEFORE ANY EARLY RETURN: a locked view is never deleted, so a path that returns
     * without unlocking would leak the whole tab. */
    for (int i = 0; i < NET_ROW_COUNT; i++) {
        if (st->rows[i]) st->rows[i]->ptrLock();
    }
    if (st->note) st->note->ptrLock();

    bool started = hg::runDetached([st, alive]() {
        NetworkProbe p = Client::networkProbe();
        brls::sync([st, alive, p]() {
            /* THE SCREEN MAY BE GONE BY NOW (build74's check, kept as a second line of defence). */
            const bool screenGone = !alive || alive->dead();

            if (!screenGone) {
            /* Every string below is built from a measurement. Where a stage was never
             * reached the row says so rather than showing a zero, because "0 bytes" and
             * "we never got that far" are different facts. */
            st->rows[NET_ROW_API]->setDetailText(p.ok ? "OK" : ("FAILED: " + p.error));

            if (p.httpStatus != 0) {
                char b[64];
                snprintf(b, sizeof b, "%ld", p.httpStatus);
                st->rows[NET_ROW_HTTP]->setDetailText(b);
            } else {
                st->rows[NET_ROW_HTTP]->setDetailText("not reached");
            }

            if (p.bodyBytes > 0) {
                char b[64];
                snprintf(b, sizeof b, "%u bytes", (unsigned)p.bodyBytes);
                st->rows[NET_ROW_BODY]->setDetailText(b);
            } else {
                st->rows[NET_ROW_BODY]->setDetailText("none");
            }

            if (p.routerBytes >= 0) {
                char b[64];
                snprintf(b, sizeof b, "%ld bytes", p.routerBytes);
                st->rows[NET_ROW_ROUTER]->setDetailText(b);
            } else {
                st->rows[NET_ROW_ROUTER]->setDetailText("not found");
            }

            {
                char b[64];
                snprintf(b, sizeof b, "%u", (unsigned)p.cardCount);
                st->rows[NET_ROW_CARDS]->setDetailText(b);
            }
            {
                char b[64];
                snprintf(b, sizeof b, "%d ms", p.msElapsed);
                st->rows[NET_ROW_ELAPSED]->setDetailText(b);
            }

            st->rows[NET_ROW_SYS_TIME]->setDetailText(hmsNow());

            if (!p.serverDate.empty()) {
                st->rows[NET_ROW_NET_TIME]->setDetailText(p.serverDate);
            } else {
                /* Absent is not the same as wrong: saying "no Date header" is what stops
                 * this row being read as a failure. */
                st->rows[NET_ROW_NET_TIME]->setDetailText("no Date header sent");
            }

            if (p.clockSkewKnown) {
                char b[96];
                /* The direction is spelled out. A console clock that is BEHIND the
                 * server is the case that breaks the signed requests, and "+3 s" versus
                 * "-3 s" is the whole diagnosis. */
                snprintf(b, sizeof b, "%+ld s (%s)", p.clockSkewSeconds,
                         p.clockSkewSeconds > 60    ? "console clock is BEHIND"
                         : p.clockSkewSeconds < -60 ? "console clock is AHEAD"
                                                    : "within a minute");
                st->rows[NET_ROW_SKEW]->setDetailText(b);
            } else {
                st->rows[NET_ROW_SKEW]->setDetailText("unknown (no server clock)");
            }

            if (st->note) {
                st->note->setText(p.ok ? "All stages passed. Press A on any row to re-run."
                                       : "A stage failed -- the row above names which one.");
            }
            hg::logPrintf("NET probe: ok=%d http=%ld body=%u router=%ld cards=%u ms=%d skew=%ld "
                          "date='%s' err='%s'",
                          (int)p.ok, p.httpStatus, (unsigned)p.bodyBytes, p.routerBytes,
                          (unsigned)p.cardCount, p.msElapsed,
                          p.clockSkewKnown ? p.clockSkewSeconds : 0, p.serverDate.c_str(),
                          p.error.c_str());
            }   /* !screenGone */

            /* UNLOCK LAST, AND ON EVERY PATH. A locked view is never deleted by borealis' pool, so missing an
             * unlock here would leak this whole tab -- including the cells the pool is holding for it. */
            for (int i = 0; i < NET_ROW_COUNT; i++) {
                if (st->rows[i]) st->rows[i]->ptrUnlock();
            }
            if (st->note) st->note->ptrUnlock();
        });
    });

    if (!started) {
        /* No worker: say so rather than leaving "running..." forever. A diagnostic that
         * hangs silently is worse than one that admits it could not run. */
        hg::logPrintf("NET: no worker for the probe -- marking it unavailable");
        st->rows[NET_ROW_API]->setDetailText("not run (no worker thread)");
        if (st->note) st->note->setText("No worker thread was available to run the probe.");
        /* AND RELEASE THE LOCKS TAKEN ABOVE: there is no callback coming on this path, so these are the only
         * unlocks it gets. */
        for (int i = 0; i < NET_ROW_COUNT; i++) {
            if (st->rows[i]) st->rows[i]->ptrUnlock();
        }
        if (st->note) st->note->ptrUnlock();
    }
}

/* ---- THE PLAY CHAIN, ON DEMAND (build157) ----------------------------------------------------------
 *
 * The automatic half lives in hg_selfcheck.cpp and runs when the player cannot get a plan or a ladder.
 * This is the other half: the same walk, asked for deliberately, with every stage on its own row.
 *
 * WHY BOTH. The automatic run answers "why did that fail, right now"; this one answers "is playback
 * working at all today", which is the question someone asks BEFORE sitting down to watch something, and
 * the one that makes a future upstream change visible without waiting to trip over it. It is also how the
 * check can be re-run after a transient failure without trying to play something.
 *
 * It is on its own tab rather than folded into the network tab because the two answer different
 * questions -- "can this console reach the internet" versus "can this console actually play a video" --
 * and because the network tab's rows are one measured HTTP fetch, while this one is six stages. */
enum SelfRowId {
    SELF_ROW_RESULT = 0,
    SELF_ROW_HOME,
    SELF_ROW_DETAIL,
    SELF_ROW_MODEL,
    SELF_ROW_LADDER,
    SELF_ROW_STREAM,
    SELF_ROW_ELAPSED,
    SELF_ROW_COUNT
};

struct SelfState {
    brls::DetailCell* rows[SELF_ROW_COUNT] = {};
    brls::Label* note = nullptr;
    /* The result row is this type, so its dot can be set without going through the base pointer. */
    class ResultDotCell* result = nullptr;
};

/* ---- THE RESULT AS A LIGHT (build158) --------------------------------------------------------------
 *
 * The user's instruction: "just show the check result as a green dot, red if it fails -- no need for this
 * text box." The text box they meant was the notification the check used to raise, and that is gone; the
 * same instruction applied to this row removes the word PASS/FAILED and leaves a circle.
 *
 * WHY IT READS A MEMBER AND NOT THE GLOBAL: this row shows the report the tab is DISPLAYING, which is
 * the last automatic one until the fresh walk that the tab starts on entry replaces it. The global status
 * would flip to the new answer while the stage rows below still showed the old one. */
class ResultDotCell : public brls::DetailCell {
  public:
    int dot = PLAY_CHAIN_UNKNOWN;

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override {
        /* The row itself first -- title, background, focus ring -- then the dot on top of it, in the
         * space the detail text would have occupied. */
        brls::DetailCell::draw(vg, x, y, width, height, style, ctx);
        if (!vg || width <= 0.0f || height <= 0.0f) return;

        NVGcolor fill;
        switch (this->dot) {
            case PLAY_CHAIN_OK:     fill = nvgRGB(64, 200, 96); break;
            case PLAY_CHAIN_FAILED: fill = nvgRGB(228, 72, 72); break;
            default:                fill = nvgRGBA(150, 150, 150, 120); break;
        }

        const float cx = x + width - 26.0f;
        const float cy = y + height * 0.5f;
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, 8.0f);
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 120));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, 7.0f);
        nvgFillColor(vg, fill);
        nvgFill(vg);
    }
};

const char* SELF_ROW_TITLES[SELF_ROW_COUNT] = {
    "Result",
    "1  Home page",
    "2  Detail page (episode id)",
    "3  Video model request (signed)",
    "4  Quality ladder",
    "5  Stream (Range request)",
    "Elapsed",
};

/* A row is one line; a measured detail can carry a whole response body, so it is cut here rather than
 * left to the cell's own layout. The FULL text is in the log either way. */
std::string rowText(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    return s.substr(0, max) + "...";
}

void fillSelfRows(SelfState* st, const PlayChainReport& rep) {
    /* Cleared first: a re-run that stops earlier than the last one must not leave the previous run's
     * later stages on screen looking like they passed. */
    for (int i = 1; i < SELF_ROW_COUNT - 1; i++) {
        if (st->rows[i]) st->rows[i]->setDetailText("not reached");
    }

    /* THE RESULT IS A DOT (build158): green passed, red failed, grey unknown. See ResultDotCell. */
    if (st->result) st->result->dot = rep.ok ? PLAY_CHAIN_OK : PLAY_CHAIN_FAILED;

    for (size_t i = 0; i < rep.stages.size(); i++) {
        const CheckStage& s = rep.stages[i];
        int row = -1;
        if (s.name == "home page") row = SELF_ROW_HOME;
        else if (s.name == "detail page") row = SELF_ROW_DETAIL;
        else if (s.name == "video model") row = SELF_ROW_MODEL;
        else if (s.name == "quality ladder") row = SELF_ROW_LADDER;
        else if (s.name == "stream") row = SELF_ROW_STREAM;
        if (row < 0) continue;
        if (st->rows[row]) {
            st->rows[row]->setDetailText((s.ok ? "OK   " : "FAIL ") + rowText(s.detail, 96));
        }
    }

    if (st->rows[SELF_ROW_ELAPSED]) {
        char b[48];
        snprintf(b, sizeof b, "%d ms", rep.msElapsed);
        st->rows[SELF_ROW_ELAPSED]->setDetailText(b);
    }

    if (st->note) st->note->setText(rowText(rep.headline, 200));
}

void runSelfCheck(SelfState* st, const AliveToken& alive) {
    /* NO TEXT ON THE RESULT ROW (build158): it is a light, and it keeps showing the last answer while the
     * next walk is in flight. Blanking it to grey for three seconds would take the answer away exactly
     * when the user came to read it. */
    if (st->note) st->note->setText("Walking the play chain: home, detail, signed request, plan, ladder, stream.");

    /* THE ROWS ARE LOCKED: this walk takes seconds and the tab can be destroyed while it runs. See the
     * note in runProbe above for the crash this prevents (an Atmosphere Data Abort inside
     * DetailCell::setDetailText, from writing into cells the tab had already freed). */
    for (int i = 0; i < SELF_ROW_COUNT; i++) {
        if (st->rows[i]) st->rows[i]->ptrLock();
    }
    if (st->note) st->note->ptrLock();

    bool started = hg::runDetached([st, alive]() {
        PlayChainReport rep = hg::runPlayChainCheck("asked from the settings screen", std::string());
        brls::sync([st, alive, rep]() {
            const bool screenGone = !alive || alive->dead();
            if (!screenGone) fillSelfRows(st, rep);

            for (int i = 0; i < SELF_ROW_COUNT; i++) {
                if (st->rows[i]) st->rows[i]->ptrUnlock();
            }
            if (st->note) st->note->ptrUnlock();
        });
    });

    if (!started) {
        hg::logPrintf("PLAYCHECK: no worker for the settings-screen run [build157]");
        if (st->rows[SELF_ROW_RESULT]) st->rows[SELF_ROW_RESULT]->setDetailText("not run (no worker)");
        if (st->note) st->note->setText("No worker thread was available to run the check.");
        for (int i = 0; i < SELF_ROW_COUNT; i++) {
            if (st->rows[i]) st->rows[i]->ptrUnlock();
        }
        if (st->note) st->note->ptrUnlock();
    }
}

brls::View* makeSelfCheckGroup(const AliveToken& alive) {
    auto* scroll = new brls::ScrollingFrame();
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(16.0f, 24.0f, 24.0f, 24.0f);

    box->addView(makeHeader("Play chain"));
    auto* rows = new SelfState();
    for (int i = 0; i < SELF_ROW_COUNT; i++) {
        if (i == 1) box->addView(makeHeader("Stages, in the order the player meets them"));
        brls::DetailCell* cell = nullptr;
        if (i == SELF_ROW_RESULT) {
            /* THE RESULT ROW IS A LIGHT (build158) -- see ResultDotCell. It starts grey ("no check has
             * run"), which is a different fact from a red that has been earned. */
            auto* dotCell = new ResultDotCell();
            dotCell->dot = PLAY_CHAIN_UNKNOWN;
            rows->result = dotCell;
            cell = dotCell;
        } else {
            cell = new brls::DetailCell();
            cell->setDetailText("-");
        }
        cell->setText(SELF_ROW_TITLES[i]);
        cell->registerClickAction([rows, alive](brls::View*) {
            runSelfCheck(rows, alive);
            return true;
        });
        box->addView(cell);
        rows->rows[i] = cell;
    }

    auto* note = new brls::Label();
    note->setFontSize(15.0f);
    note->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
    note->setMarginTop(10.0f);
    note->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    note->setText("Press A on any row to walk the chain again. The same check runs by itself when a plan "
                  "or a quality list fails, and reports the server's own answer.");
    box->addView(note);
    rows->note = note;

    scroll->setContentView(box);

    /* SHOW THE LAST AUTOMATIC RESULT IMMEDIATELY, THEN RE-RUN. If the player has just failed, that report
     * is the answer the user came here for, and it is shown before the (slower) fresh walk replaces it. */
    {
        PlayChainReport last;
        if (hg::lastPlayChainReport(last)) fillSelfRows(rows, last);
    }
    runSelfCheck(rows, alive);
    return scroll;
}

brls::View* makeNetworkGroup(const AliveToken& alive) {
    auto* scroll = new brls::ScrollingFrame();
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(16.0f, 24.0f, 24.0f, 24.0f);

    box->addView(makeHeader("Connection"));
    /* Allocated once and owned by the lambda captures below: it has to outlive this
     * function, because the probe's brls::sync callback writes into it after the tab
     * has been returned. borealis owns the VIEWS; this small struct is the handle they
     * are reached through, and it is captured by value everywhere it is used. */
    auto* rows = new NetState();
    for (int i = 0; i < NET_ROW_COUNT; i++) {
        if (i == 3) box->addView(makeHeader("API test (home page, end to end)"));
        if (i == 9) box->addView(makeHeader("Clocks"));
        auto* cell = new brls::DetailCell();
        cell->setText(NET_ROWS[i].title);
        cell->setDetailText(NET_ROWS[i].initial);
        /* EVERY ROW IS A RE-RUN BUTTON. wiliwili puts one test button at the top; making
         * every row trigger it means the user does not have to scroll back up to try
         * again, and on a console that is the difference between a diagnostic and a
         * chore. */
        cell->registerClickAction([rows, alive](brls::View*) {
            runProbe(rows, alive);
            return true;
        });
        box->addView(cell);
        rows->rows[i] = cell;
    }

    auto* note = new brls::Label();
    note->setFontSize(15.0f);
    note->setTextColor(brls::Application::getTheme().getColor("brls/text_disabled"));
    note->setMarginTop(10.0f);
    note->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    note->setText("Press A on any row to run the test again.");
    box->addView(note);
    rows->note = note;

    /* The static readings first: they need no network and they answer "is the console
     * online at all" while the probe's slower answer is still coming. */
    fillNetworkBasics(rows);

    scroll->setContentView(box);
    /* Kicked off here, once, so the tab has an answer ready by the time it is looked
     * at. It runs on a worker thread, so building the tab does not block. */
    runProbe(rows, alive);
    return scroll;
}

} // namespace

SettingsActivity::SettingsActivity() = default;

SettingsActivity::~SettingsActivity() {
    /* STOP THE NETWORK PROBE FROM WRITING INTO THIS SCREEN AFTER IT IS GONE. The probe takes seconds and its
     * callback writes into rows this activity owns; leaving the screen during it used to write into freed
     * cells, which is the crash the console reported on this screen. See hg_alive.hpp and K8. */
    if (this->alive_) this->alive_->kill();
}

brls::View* SettingsActivity::createContentView() {
    hg::logPrintf("SETTINGS: ENTER createContentView (six sections, network probe guarded) [build74]");

    auto* tabs = new brls::TabFrame();

    /* ONE TOKEN FOR THE WHOLE SCREEN, handed to the tab builders that need it -- the network probe's callback
     * writes into these rows after a fetch that takes seconds. See hg_alive.hpp. */
    const AliveToken alive = this->alive_;

    /* One tab per group, exactly like wiliwili's screen: a header, then rows. The
     * Options tab is built from the same table the rows are, so adding a row to the
     * table adds it to the screen and nothing else has to be kept in step. */
    for (size_t i = 0; i < sizeof GROUPS / sizeof GROUPS[0]; i++) {
        const Group g = GROUPS[i];
        /* The tab label is read ONCE, while the tabs are built: this screen is rebuilt whenever it is\n         * pushed, and the language only changes on a restart, so a lambda is not needed here. */
        tabs->addTab(groupTitle(g), [g]() { return makeSettingGroup(g); });
    }
    /* The network tab goes before About: About is the last thing anyone reads, and the
     * diagnostic is the one tab that is useful when something is wrong. */
    /* The Network tab carries BOTH the connection readout and the HTTP settings, so its label comes\n     * from the same table as the other tabs. */
    tabs->addTab(hg_tr(HG_T_SETTINGS_NETWORK), [alive]() { return makeNetworkGroup(alive); });

    /* THE PLAY-CHAIN TAB SITS NEXT TO THE NETWORK ONE, for the same reason the network tab sits before
     * About: "can this console play a video" is the question a diagnostic exists to answer, and it is
     * six stages that the network tab does not cover (the signed request, the plan, the ladder, the
     * stream). Its label is a plain English string rather than an hg_tr key, like the rest of this
     * screen's remaining strings -- see the note about the seven untranslated ones in the handover. */
    tabs->addTab("Play check", [alive]() { return makeSelfCheckGroup(alive); });

    /* ---- 关于更多 AND 开源许可 ARE GONE, AT THE USER'S REQUEST -----------------------------------------
     *
     * Their instruction (2026-10-10): "delete 关于更多 and 开源许可 as well, they are not needed". Both were built
     * by free functions that are still in this file, so putting a tab back is one line each -- which is why the
     * builders were left where they are rather than deleted with the tabs.
     *
     * A NOTE FOR WHOEVER DISTRIBUTES THIS BUILD: the list on that tab was the GPL/LGPL notice of everything this
     * binary is built from, and those notices are a requirement of *distribution* rather than of use. They are
     * preserved in `docs/第三方组件与许可.md` for that reason. This app is a personal build that the person who
     * asked for the removal runs themselves; if it is ever handed to someone else, put the tab back (one line) or
     * ship that file with it. */

    hg::logPrintf("SETTINGS: %u tabs built (about and licences removed at the user's request) [build81]",
                  (unsigned)(sizeof GROUPS / sizeof GROUPS[0] + 1));

    /* B LEAVES THE SETTINGS SCREEN, AND IT HAS TO BE REGISTERED ON THE TAB FRAME.
     *
     * MEASURED ON HARDWARE: "from the settings screen you cannot get back to the home page -- it reports
     * a software error and crashes", with NOTHING in the log because the process died before any logger
     * could write.
     *
     * There was no back handling here at all, and borealis' Activity registers no back action of its own
     * (the only B actions in the library are in dialog.cpp), so the press went past this screen.
     *
     * Registered on `tabs` rather than on the activity: `handleAction` looks the action up starting from
     * the FOCUSED view and walking up its parents, so a registration on the Activity itself is not in that
     * chain while every row and sidebar item inside the frame is. */
    tabs->registerAction(hg_tr(HG_T_BACK), brls::ControllerButton::BUTTON_B, [](brls::View*) {
        hg::logPrintf("SETTINGS: B pressed -> popActivity (deferred one frame)");
        /* Deferred for the same reason as the detail and player screens: popActivity destroys this
         * Activity AND the view the action is registered on, while the dispatcher is still walking it.
         * brls::sync is right here because the main loop finishes dispatching in Application::frame()
         * before it calls Threading::performSyncTasks() (application.cpp:201-204). */
        brls::sync([]() { brls::Application::popActivity(); });
        return true;
    });

    return hg::withHintBar(tabs);
}

} // namespace hg
