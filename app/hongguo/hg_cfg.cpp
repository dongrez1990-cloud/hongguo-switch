/*
 * hg_cfg implementation. See hg_cfg.hpp.
 *
 * GPL-3.0-only.
 *
 * ASCII-only source file.
 */

#include "hg_cfg.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hg_log.hpp"

namespace hg {

static const char* PATHS[] = {"sdmc:/switch/hongguo.cfg", "sdmc:/hongguo.cfg"};

static std::string g_text;
static bool g_read = false;
static bool g_logged = false;

/* Which candidate path was actually read, so cfgWrite edits THAT file. Writing to
 * sdmc:/switch/hongguo.cfg when the values in force came from sdmc:/hongguo.cfg
 * would leave the app on the old values while the settings screen said otherwise. */
static const char* g_path = nullptr;

const std::string& cfgText() {
    if (g_read) return g_text;
    g_read = true;

    for (size_t i = 0; i < sizeof PATHS / sizeof PATHS[0]; i++) {
        std::FILE* f = std::fopen(PATHS[i], "r");
        if (!f) continue;

        /* READ TO EOF, NOT ONE BUFFERFUL. An earlier version read a single
         * 1023-byte chunk, and the config file's own comment header pushed the
         * actual `ao=null` line past that cut -- so the run silently used the
         * built-in default instead, while the log said "file present but
         * contains no key=value lines", which was true only of the truncated
         * part. The measurement was of the wrong arm and looked like a result. */
        g_text.clear();
        char buf[512];
        size_t n;
        bool truncated = false;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
            if (g_text.size() + n > MAX_CFG_BYTES) {
                truncated = true;
                break;
            }
            g_text.append(buf, n);
        }
        std::fclose(f);

        if (truncated) {
            hg::logPrintf("CFG: %s is larger than %u bytes and was truncated -- options after "
                          "that point are NOT in force",
                          PATHS[i], (unsigned)MAX_CFG_BYTES);
        }
        hg::logPrintf("CFG: read %s (%u bytes)", PATHS[i], (unsigned)g_text.size());
        /* Remembered so cfgWrite edits the file the values actually came from.
         * Two candidate paths exist, so writing to the wrong one would leave the
         * app on the old values while the settings screen claimed otherwise. */
        g_path = PATHS[i];
        return g_text;
    }
    hg::logPrintf("CFG: no %s (all options at their built-in defaults)", PATHS[0]);
    return g_text;
}

/* Iterates lines without allocating. Calls fn(key, value) for each `key=value`
 * line whose key is non-empty, after trimming spaces and CR. */
template <typename Fn>
static void forEachPair(Fn fn) {
    const std::string& text = cfgText();
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;

        while (!line.empty() && (line[0] == ' ' || line[0] == '\t')) line.erase(0, 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                 line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
        if (key.empty()) continue;

        fn(key, val);
    }
}

std::string cfgValue(const char* key, const char* fallback) {
    std::string found;
    bool have = false;
    forEachPair([&](const std::string& k, const std::string& v) {
        if (have) return;
        if (k == key) {
            found = v;
            have = true;
        }
    });
    return have ? found : std::string(fallback);
}

int cfgIntOr(const char* key, int fallback) {
    std::string raw = cfgValue(key, "");
    if (raw.empty()) return fallback;
    char* end = nullptr;
    long v = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0') {
        hg::logPrintf("CFG: %s='%s' is not a whole number -- using %d", key, raw.c_str(), fallback);
        return fallback;
    }
    return (int)v;
}

double cfgDoubleOr(const char* key, double fallback) {
    std::string raw = cfgValue(key, "");
    if (raw.empty()) return fallback;
    char* end = nullptr;
    double v = strtod(raw.c_str(), &end);
    if (end == raw.c_str() || *end != '\0') {
        hg::logPrintf("CFG: %s='%s' is not a number -- using %.2f", key, raw.c_str(), fallback);
        return fallback;
    }
    return v;
}
bool cfgIs(const char* key, const char* value) {
    std::string got;
    bool have = false;
    forEachPair([&](const std::string& k, const std::string& v) {
        if (have) return;
        if (k == key) {
            got = v;
            have = true;
        }
    });
    if (have) return got == value;
    /* Shipping defaults.  CFG remains an override for experiments, but a
     * clean NSP must use the same hardware path as the distributed CFG. */
    if (std::strcmp(key, "renderer") == 0) return std::strcmp(value, "deko3d") == 0;
    if (std::strcmp(key, "renderctx") == 0) return std::strcmp(value, "no") == 0 ? false : true;
    return false;
}

void cfgLogOnce() {
    if (g_logged) return;
    g_logged = true;
    int n = 0;
    forEachPair([&](const std::string& k, const std::string& v) {
        n++;
        hg::logPrintf("CFG: option %s = '%s'", k.c_str(), v.c_str());
    });
    if (n == 0) hg::logPrintf("CFG: file present but contains no key=value lines");
}

/* ---- writing ------------------------------------------------------------- */

bool cfgWrite(const char* key, const char* value) {
    if (key == nullptr || *key == '\0') {
        hg::logPrintf("CFG write: refusing an empty key");
        return false;
    }

    /* Force a read so g_path is known, and so a change made over FTP since
     * startup is not overwritten by a stale cache. */
    cfgText();
    const char* path = g_path;
    if (path == nullptr) {
        /* Nothing was readable. Fall back to the primary path so the very first
         * setting change creates the file rather than failing. */
        path = PATHS[0];
        hg::logPrintf("CFG write: no file was readable; creating %s", path);
    }

    const std::string& old = g_text;

    /* REBUILD LINE BY LINE. Every line that is not this key is copied through
     * verbatim, which is what preserves the comment header -- the file documents
     * its own options, and a settings screen that deleted that would be trading
     * one kind of knowledge for another. */
    std::string out;
    out.reserve(old.size() + strlen(key) + strlen(value) + 8);
    bool replaced = false;
    size_t pos = 0;
    while (pos <= old.size()) {
        size_t end = old.find('\n', pos);
        bool last = (end == std::string::npos);
        if (last) end = old.size();
        std::string line = old.substr(pos, end - pos);
        pos = end + 1;

        std::string probe = line;
        while (!probe.empty() && (probe[0] == ' ' || probe[0] == '\t')) probe.erase(0, 1);
        size_t eq = probe.find('=');
        bool isOurs = false;
        if (!probe.empty() && probe[0] != '#' && eq != std::string::npos) {
            std::string k = probe.substr(0, eq);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            isOurs = (k == key);
        }

        if (isOurs) {
            if (!replaced) {
                out += key;
                out += '=';
                out += value;
                replaced = true;
            }
            /* A duplicate of the same key is DROPPED rather than kept: cfgValue
             * takes the first match, so leaving a later duplicate would make the
             * file disagree with the value in force. */
        } else {
            out += line;
        }
        if (!last) out += '\n';
    }
    if (!replaced) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += key;
        out += '=';
        out += value;
        out += '\n';
    }

    std::string tmp = std::string(path) + ".tmp";
    std::string bak = std::string(path) + ".bak";
    {
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) {
            hg::logPrintf("CFG write: cannot open %s for writing", tmp.c_str());
            return false;
        }
        size_t wrote = std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        if (wrote != out.size()) {
            hg::logPrintf("CFG write: %s got %u of %u bytes; leaving the old file alone",
                          tmp.c_str(), (unsigned)wrote, (unsigned)out.size());
            std::remove(tmp.c_str());
            return false;
        }
    }

    /* ROTATE BEFORE RENAMING. A console can lose power at any moment. Renaming the
     * new file directly over the old one would, on FAT, mean deleting the old one
     * first -- and a power loss in that window leaves no settings file at all.
     * With a rotation there is always a complete file under one of the names. */
    std::remove(bak.c_str());
    if (std::rename(path, bak.c_str()) != 0) {
        hg::logPrintf("CFG write: could not rotate %s to .bak (continuing anyway)", path);
    }
    if (std::rename(tmp.c_str(), path) != 0) {
        hg::logPrintf("CFG write: rename %s -> %s FAILED; restoring the old file",
                      tmp.c_str(), path);
        std::rename(bak.c_str(), path);
        std::remove(tmp.c_str());
        return false;
    }

    /* The cache must not outlive the file it came from. */
    g_text = out;
    g_read = true;
    g_logged = false;   /* let the next cfgLogOnce state the new arm */

    hg::logPrintf("CFG write: %s=%s written to %s (%u bytes)", key, value, path,
                  (unsigned)out.size());
    return true;
}

} // namespace hg
