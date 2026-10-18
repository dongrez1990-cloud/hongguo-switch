/*
 * hg_log: a boot breadcrumb log for the Switch UI application.
 *
 * GPL-3.0-only.
 *
 * WHY THIS EXISTS. A crash on the console shows a plain error dialog and
 * nothing else: there is no screen to read, no core dump to pull, and the
 * previous session lost whole console trips to experiments that answered only
 * "it still crashes". The fix is to make the program leave a trail, so ONE run
 * answers questions that otherwise cost one run each.
 *
 * The protocol is deliberately dumb: every line is written and flushed
 * IMMEDIATELY, and the interesting line is the last one. "ENTER foo" is written
 * BEFORE foo runs, so a log that ends at "ENTER foo" says exactly where the
 * program died. No line is ever buffered, because a crash does not flush.
 *
 * The file is sdmc:/switch/hongguo.log (with sdmc:/hongguo.log as a fallback),
 * so it can be pulled back over FTP without touching the console -- and it
 * survives both an abort and a hard crash, which a captured stdout does not.
 *
 * Everything is mirrored to stdout as well, so launching under nxlink -s shows
 * the same trail live.
 *
 * ASCII-only source file.
 */

#pragma once

/* The breadcrumb log is APPENDED across sessions, so it needs a cap: above this it rotates (the previous run
 * is then discarded, which the new file says in its first line). 512 KiB is roughly a hundred sessions. */
#define HG_LOG_MAX_BYTES (512u * 1024u)

/* HOW MUCH MEMORY THIS PROCESS IS USING, in KiB, or -1 when the question cannot be answered.
 *
 * WHY IT IS WORTH A LOG LINE (the user asked, after several crashes on the episode screen: "is it too much
 * stuff now, is memory overflowing, is a cache never being reset?"). That is a testable question and this makes
 * it a NUMBER in the same file every other diagnosis comes from: compare the line at boot with the lines during
 * playback across a long session and growth is either there or it is not.
 *
 * It is the HEAP in use, from mallinfo() -- a libc call, so it cannot panic the process. (An earlier version used a
 * syscall with an InfoType that belongs to a different syscall, and the kernel rightly panicked: see hg_log.cpp.) */
long hgMemoryUsedKB(void);

#include <cstdio>

namespace hg {

/* Opens the log and points borealis' own logger at the same file. Called as
 * the FIRST statement of main(), before borealis starts, so that everything
 * after it is recorded. If the file cannot be created the log degrades to
 * stdout only and says so. */
void logOpen(const char* tag);

/* One breadcrumb. Always flushed, always timestamped. */
void logPrintf(const char* fmt, ...);

/* The open log file, or nullptr when only stdout is available. Used to hand
 * borealis' logger the same destination. */
std::FILE* logFile();

/* Writes a final line and closes. Safe to call twice. */
void logClose();

/* Installs a std::terminate handler that records the reason before the process
 * dies. This is the one crash path that can be turned into evidence for free:
 * an uncaught exception is what brls::fatal() raises on this platform, and it
 * is also what a failed std::thread construction raises -- both would
 * otherwise present as a bare abort with no message at all. */
void installCrashRecorder();

} // namespace hg
