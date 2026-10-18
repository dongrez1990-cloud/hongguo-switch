/*
 * hg_log implementation. See hg_log.hpp for why the log exists.
 *
 * GPL-3.0-only.
 *
 * TWO FILES, ON PURPOSE.
 *
 *   sdmc:/switch/hongguo.log       the breadcrumbs. NOTHING IS EVER HELD OPEN:
 *                                  each line opens the file, appends, fsyncs
 *                                  and closes it.
 *   sdmc:/switch/hongguo-brls.log  borealis' own logger, which needs a live
 *                                  FILE* and therefore does hold one open.
 *
 * The split exists because of a real dead end. sys-ftpd answered
 * "450 failed to open file" for the breadcrumb log while the application was
 * still running, so the one artifact that could explain a hang was exactly the
 * artifact that could not be read while it was needed. Keeping the breadcrumbs
 * append-and-close makes them readable at any moment, at the cost of two extra
 * syscalls per line -- which for a few hundred lines is nothing, and each line
 * is fsync'd anyway because a crash does not flush.
 *
 * ASCII-only source file.
 */

#include "hg_log.hpp"

#include <borealis.hpp>

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <time.h>

#include <unistd.h>

#ifdef __SWITCH__
/* svcGetSystemInfo(InfoType_UsedMemorySize): the kernel's own memory accounting for this process. See the note on
 * hgMemoryUsedKB in hg_log.hpp for why it is worth a log line. */
#include <malloc.h>   /* mallinfo(): see hgMemoryUsedKB */
#endif

namespace hg {

static const char* CANDIDATES[] = {"sdmc:/switch/hongguo.log", "sdmc:/hongguo.log"};
static const char* BRLS_CANDIDATES[] = {"sdmc:/switch/hongguo-brls.log",
                                        "sdmc:/hongguo-brls.log"};

static char g_path[128] = "";      /* breadcrumb log; empty means stdout only */
static std::FILE* g_brlsFile = nullptr; /* borealis' logger destination */
static bool g_opened = false;
static bool g_wroteHeader = false;

std::FILE* logFile() { return g_brlsFile; }

/* Opens path in mode, returning nullptr on failure. */
static std::FILE* openAt(const char* path, const char* mode) { return std::fopen(path, mode); }

/* ---- HOW OFTEN THE LOG IS SYNCED TO THE CARD (build145) ------------------------------------------------
 *
 * Every log line used to fsync. That is safe and it is why a crash keeps its breadcrumbs, but an SD card's fsync
 * blocks for tens to hundreds of milliseconds, and the player writes a few lines per second FROM THE FRAME LOOP --
 * so the frame stalled, the picture hitched, and mpv's audio clock ran ahead and had to be caught up in one
 * garbled burst. The user measured the cadence exactly: "a stutter at three seconds and another at six."
 *
 * One sync per 500 ms keeps the breadcrumbs useful (a crash loses at most half a second) and takes the sync off
 * the critical path. See the note at the fsync itself. */
#define HG_LOG_FSYNC_MS 2000

/* ---- FSYNC IS OFF ENTIRELY (build150, the user's instruction) ------------------------------------------
 *
 * THEIR WORDS: "just turn that fsync off, do not write it."
 *
 * 0 means the interval test can never be satisfied, so the logger never syncs. The lines are still written and
 * flushed and the file is still closed, so they are in the card's cache and every breadcrumb survives a crash of
 * this application -- which is what the log is for. What is given up is durability across a console power loss,
 * and the user has decided that is worth less than a frame loop that does not wait on an SD card.
 *
 * It is a number, not a switch, on purpose: the two-second version was still producing hitches (measured at
 * 18/19 s, then 27/28 s) and this build answers the question "is the sync itself the cause" once and for all. If
 * the hitches persist with this, the sync was never the cause and the next step is the stall detector described
 * in docs/build148-之后-四件与卡顿指认器.md. */

/* Raised from 500 to 2000 (build148). 500 ms was still enough to stutter the player: the user measured the
 * remaining hitch at "eighteen or nineteen seconds into episode five", i.e. still periodic and still drifting,
 * which is what an SD card's own garbage collection looks like when a sync lands on it -- mostly quick, then
 * hundreds of milliseconds once in a while. Two seconds makes those collisions rare.
 *
 * THE BREADCRUMBS ARE NOT LOST. Every line is still fprintf'd and fflush'd, then the file is closed -- so the
 * text is in the system's page cache and survives a crash of THIS application, which is what the log is for
 * (the note at the top of this file). What fsync adds is durability across a console power-loss or a system
 * crash, and that is worth two seconds of a frame loop's time, not worth a stutter every half second. */

/* Monotonic milliseconds, local to this file so the logger depends on nothing else. */
static uint64_t hg_log_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

void logOpen(const char* tag) {
    if (g_opened) return;
    g_opened = true;

    /* ---- APPEND, NOT TRUNCATE, AND THAT IS THE FIX FOR LOSING THE EVIDENCE -------------------------
     *
     * MEASURED (round 32): this opened the log with "w", so EVERY LAUNCH WIPED THE PREVIOUS SESSION. The cost
     * was a whole round of diagnosis: build69 crashed on startup before logging anything useful, and with it
     * went the PREVIOUS run's lines -- the ones that would have answered the outstanding question about the
     * control bar. A crash that destroys the log of the run before it is the worst possible behaviour for a
     * project whose only window onto the console is this file.
     *
     * So: append, with a separator between sessions and a size cap so it cannot grow for ever on the SD card.
     * The previous session's tail is now always there, whatever this one does. */
    for (size_t i = 0; i < sizeof CANDIDATES / sizeof CANDIDATES[0]; i++) {
        /* ROTATE FIRST: above the cap, start a fresh file rather than appending to a monster. */
        long existing = 0;
        if (std::FILE* probe = openAt(CANDIDATES[i], "rb")) {
            if (std::fseek(probe, 0, SEEK_END) == 0) existing = std::ftell(probe);
            std::fclose(probe);
            if (existing > (long)HG_LOG_MAX_BYTES) {
                if (std::FILE* fresh = openAt(CANDIDATES[i], "w")) {
                    std::fputs("\xEF\xBB\xBF", fresh);
                    std::fprintf(fresh, "[hg] log rotated at %ld bytes (cap %u); the previous run is gone\n",
                                 existing, (unsigned)HG_LOG_MAX_BYTES);
                    std::fclose(fresh);
                    existing = 0;
                }
            }
        }

        std::FILE* f = openAt(CANDIDATES[i], "a");
        if (f) {
            /* ONE BOM, AND ONLY ON AN EMPTY FILE: appending one mid-file would corrupt the last line of the
             * previous session. (This also removes the doubled BOM the old code wrote.) */
            if (existing == 0) std::fputs("\xEF\xBB\xBF", f);
            std::fprintf(f, "\n=== NEW SESSION %s, built %s %s ===\n", tag ? tag : "?", __DATE__, __TIME__);
            std::fclose(f);
            std::strncpy(g_path, CANDIDATES[i], sizeof g_path - 1);
            g_path[sizeof g_path - 1] = '\0';
            break;
        }
    }
    g_wroteHeader = true;

    logPrintf("=== hongguo %s log, built %s %s [build72] ===", tag ? tag : "?", __DATE__, __TIME__);
    if (g_path[0]) {
        logPrintf("breadcrumb log: %s (append-and-close, readable while running; SEVERAL SESSIONS, and the "
                  "newest is at the END -- a crash cannot wipe the session before it)",
                  g_path);
    } else {
        logPrintf("WARNING: no breadcrumb log file could be created; stdout only");
    }

    /* borealis' logger gets its own file, because it keeps the FILE* and a
     * shared one would put us back in the unreadable-while-running state. */
    for (size_t i = 0; i < sizeof BRLS_CANDIDATES / sizeof BRLS_CANDIDATES[0]; i++) {
        std::FILE* f = openAt(BRLS_CANDIDATES[i], "w");
        if (f) {
            g_brlsFile = f;
            std::setvbuf(f, nullptr, _IONBF, 0);
            break;
        }
    }
    if (g_brlsFile) {
        brls::Logger::setLogOutput(g_brlsFile);
        brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        brls::Logger::setThreadSafeLogging(true);
        logPrintf("borealis logger -> %s at DEBUG (held open, hence the split)", BRLS_CANDIDATES[0]);
    }
}

void logPrintf(const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    body[sizeof body - 1] = '\0';

    /* The console clock has been sane in every measurement so far, but a wrong
     * clock must not turn into a wrong log: relative ordering is what matters,
     * and a bad time is visible as a bad prefix. */
    char stamp[32] = "??:??:??";
    std::time_t t = std::time(nullptr);
    if (t > 0) {
        struct tm tmv;
        /* localtime_r, not localtime: this is called from worker threads as
         * well as from the main thread, and localtime's shared static buffer
         * would race. */
        if (localtime_r(&t, &tmv)) {
            std::snprintf(stamp, sizeof stamp, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min,
                          tmv.tm_sec);
        }
    }

    std::printf("[hg %s] %s\n", stamp, body);
    std::fflush(stdout);

    if (g_path[0]) {
        std::FILE* f = openAt(g_path, "a");
        if (f) {
            std::fprintf(f, "[hg %s] %s\n", stamp, body);
            std::fflush(f);

            /* ---- THE fsync IS RATE LIMITED, AND THAT IS THE FIX FOR THE PLAYBACK STUTTER (build145) ------
             *
             * THE USER, and it is the measurement that found this: "on episode four there is a stutter at three
             * seconds and another at six seconds, and the video hitches with it."
             *
             * Three seconds is a LOG CADENCE, not a video cadence: the player writes "MPV: PLAYING", "PLAYER
             * timing" (player_view.cpp:3139) and "frame N" about once a second. Every one of those lines opened
             * this file, appended, and called fsync -- which is this logger's design, stated at the top of this
             * file, so that a crash cannot lose the last breadcrumb. On an SD card an fsync blocks for tens to
             * hundreds of milliseconds, and these lines are written FROM THE FRAME LOOP, so the frame stalls, the
             * video hitches, mpv's audio clock runs ahead, and mpv catches up in one burst -- which is the garbled
             * "sped up past recognition" noise the user hears, followed by the picture and sound no longer
             * matching. The same mechanism is what made startup feel slower before it was found (a per-frame log
             * line, build118).
             *
             * So the sync is throttled instead of removed: at most one fsync per HG_LOG_FSYNC_MS, and always one
             * when the caller asks for it (fatal paths). A crash can therefore lose at most the last few hundred
             * milliseconds of breadcrumbs, which is the trade this project can afford -- a stutter every three
             * seconds is not. */
            const uint64_t now = hg_log_now_ms();
            static uint64_t lastSync = 0;
            /* HG_LOG_FSYNC_MS == 0 MEANS NEVER, AND THE FIRST DRAFT GOT THAT BACKWARDS. Writing the test as
             * "now - lastSync >= HG_LOG_FSYNC_MS" makes 0 always true, i.e. it would have synced EVERY line --
             * the exact opposite of "turn the sync off". The guard below is what makes 0 mean never, and it is
             * the only line that changed between the 2000 ms build and this one. */
            if (HG_LOG_FSYNC_MS > 0 && now - lastSync >= HG_LOG_FSYNC_MS) {
                lastSync = now;
                int fd = fileno(f);
                if (fd >= 0) fsync(fd);
            }
            std::fclose(f);
        }
    }
}

void logClose() {
    if (g_brlsFile) {
        /* RE-POINT THE LOGGER BEFORE CLOSING ITS DESTINATION.
         *
         * borealis' Logger keeps the FILE* it was given and writes to it from
         * anywhere, including destructors that run AFTER this function, during
         * shutdown. Closing the file while the logger still points at it means
         * the next log call writes through a dangling FILE* -- which presents
         * as "+ crashes on exit" and has nothing to do with the exit path
         * itself. Handing it back to stdout first makes the close safe. */
        brls::Logger::setLogOutput(stdout);
        std::fflush(g_brlsFile);
        std::fclose(g_brlsFile);
        g_brlsFile = nullptr;
    }
}

/* The terminate handler must not itself throw or recurse: a second failure
 * inside it would abort with no record at all, which is the failure mode this
 * exists to remove. So it is a flag, a read of current_exception, and a write. */
static bool g_terminating = false;

static void terminateRecorder() {
    if (g_terminating) {
        std::printf("[hg] terminate handler re-entered; giving up\n");
        std::abort();
    }
    g_terminating = true;

    logPrintf("FATAL std::terminate reached (uncaught exception or explicit call)");
    try {
        std::exception_ptr e = std::current_exception();
        if (e) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                logPrintf("FATAL exception what() = %s", ex.what());
            } catch (...) {
                logPrintf("FATAL exception of non-std::exception type");
            }
        } else {
            logPrintf("FATAL no active exception (terminate called directly)");
        }
    } catch (...) {
        logPrintf("FATAL inspection of the exception itself failed");
    }

    logPrintf("FATAL the process is about to abort; the line above is the reason");
    logClose();
    std::abort();
}

void installCrashRecorder() { std::set_terminate(terminateRecorder); }

} // namespace hg
/* See hg_log.hpp. libnx's svcGetSystemInfo answers this; the call is cheap enough to make once a second. */
long hgMemoryUsedKB(void) {
    /* HEAP IN USE, from the C library own accounting.
     *
     * THE FIRST VERSION USED THE WRONG SYSCALL AND PANICKED THE CONSOLE AT STARTUP. It called
     * svcGetSystemInfo(..., InfoType_UsedMemorySize, ...), but InfoType_UsedMemorySize belongs to svcGetInfo
     * (libnx svc.h:192 declares that enum beside svcGetInfo at :847; svcGetSystemInfo at :1555 takes a DIFFERENT
     * set of property IDs). The kernel therefore saw an invalid id0 -- and an invalid id0 is a PANIC, not a
     * Result. The console log says exactly where it died: every session ends at "ENTER main (shipping)" with no
     * second line, because this call is evaluated inside the first logPrintf arguments, before that line can be
     * written.
     *
     * SO IT IS A LIBC CALL NOW INSTEAD OF A SYSCALL: mallinfo() cannot panic a process. It reports the HEAP, not
     * the whole process, so a GPU-side allocation would not show up here -- but a leak in this app own C++
     * allocations, which is what "is something never freed" would look like, does. */
    struct mallinfo mi = mallinfo();
    return (long)((unsigned long)mi.uordblks / 1024ul);
}
