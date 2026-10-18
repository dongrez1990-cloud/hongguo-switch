/*
 * hg_cfg: a tiny key=value file read from the SD card, so an experiment can be
 * flipped over FTP instead of costing a rebuild and a console trip.
 *
 * GPL-3.0-only.
 *
 * WHY. Every open question in this project has the same shape: "is it this
 * value or that one?". Answering each by rebuilding costs a compile, a 26 MB
 * upload and a physical trip to the console, and it changes the binary at the
 * same time as the variable, which is two things at once. A file that is read
 * at startup turns that into an FTP write.
 *
 *   sdmc:/switch/hongguo.cfg
 *     ao=null                -- audio output; default "hos"
 *     fetch=sync             -- resolve the home page on the main thread
 *     hwdec=no               -- decode without hardware
 *
 * Anything absent falls back to the caller's default, so the file is optional
 * and a missing or malformed file is never fatal -- it is logged instead.
 *
 * ASCII-only source file.
 */

#pragma once

#include <string>

namespace hg {

/* Hard ceiling on the config file. Large enough that no realistic comment
 * header can push a real setting out of range -- the 1 KiB buffer this replaced
 * did exactly that, and the loss was silent. */
const size_t MAX_CFG_BYTES = 16384;

/* The whole config file as text, read once and cached. Empty if unreadable. */
const std::string& cfgText();

/* The value of `key`, or `fallback` when the key is absent or has no value.
 * The comparison is exact and case-sensitive; leading spaces and a trailing CR
 * are trimmed, because the file is usually written from Windows. */
std::string cfgValue(const char* key, const char* fallback);

/* True when `key` is present and its value equals `value`. Exact match, not a
 * substring: "async" contains "sync", and a substring match on a bisect switch
 * would silently test the wrong arm. */
bool cfgIs(const char* key, const char* value);

/* An integer or a real, with a fallback for "absent" AND for "not a number". Both log when they refuse a
 * value, because a cfg line that is silently ignored is the same class of bug as a key that does nothing. */
int cfgIntOr(const char* key, int fallback);
double cfgDoubleOr(const char* key, double fallback);

/* Logs every key=value pair found, once. Called for the diagnostic record so a
 * run states which arm it was on. */
void cfgLogOnce();

/* Sets `key` to `value` in the config file, preserving every other line --
 * including the comment header, which is the file's own documentation.
 *
 * WHY THIS LIVES HERE RATHER THAN IN THE SETTINGS SCREEN. The settings screen and
 * the FTP experiment switches must be the SAME mechanism, not two stores that can
 * disagree. A setting changed on the console that the next `cfgValue()` call does
 * not see would be worse than no settings screen.
 *
 * An existing `key=` line is replaced in place; otherwise `key=value` is appended.
 * The cache is invalidated and re-read, so the value is live immediately for the
 * keys that are read at use time (those read once at startup -- ui.scale is one --
 * need a restart, which the caller is responsible for saying).
 *
 * Written the careful way: the new text goes to a temporary file, the old file is
 * rotated to .bak, and the temporary is renamed into place. A console can lose
 * power at any moment and a half-written settings file would be a bricked app
 * with no explanation. Returns false when nothing could be written, and says why
 * in the log. */
bool cfgWrite(const char* key, const char* value);

} // namespace hg
