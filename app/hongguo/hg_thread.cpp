/*
 * hg_thread implementation. See hg_thread.hpp for why libnx and not std::thread.
 *
 * GPL-3.0-only.
 *
 * ASCII-only source file.
 */

#include "hg_thread.hpp"

#include <switch.h>

#include <atomic>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include "hg_log.hpp"
#include "hg_http.h"   /* hg_http_in_flight: see waitForHttpIdle (build168) */

namespace hg {

/* LIFETIME. threadCreate() allocates the stack when stack_mem is NULL, and
 * threadClose() is what frees it -- but only after the thread has exited. A
 * detached thread therefore cannot clean up after itself, so every worker is
 * registered and the finished ones are reaped on the main thread. Without this
 * each worker leaks its stack, and this app starts up to four (home fetch,
 * detail fetch, play-plan fetch, mpv state poller).
 *
 * One heap object carries the handle, the callable and the done flag, so the
 * reaper can release everything with a single delete.
 *
 * The mutex is std::mutex, which does work here: it reduces to
 * pthread_mutex_lock, and the mutexes borealis' logger and brls::sync use are
 * demonstrably functional on this platform (the logger produced intact lines
 * from the worker thread, and all of them arrived). */
struct Worker {
    Thread thread;
    std::function<void()> fn;
    std::atomic<bool> done{false};
};

static std::mutex g_workersMutex;
static std::vector<Worker*> g_workers;

static void workerEntry(void* arg) {
    Worker* w = (Worker*)arg;
    w->fn();
    w->done.store(true);
    /* Nothing may touch w after this store: the main thread is free to reap and
     * delete it as soon as it observes done. threadWaitForExit in the reaper is
     * what guarantees the thread has actually left this function first. */
}

/* Closes every worker that has finished. Called on the main thread, before a
 * new one is started and from the main loop. */
static void reapFinishedWorkers() {
    std::lock_guard<std::mutex> lock(g_workersMutex);
    for (size_t i = 0; i < g_workers.size();) {
        Worker* w = g_workers[i];
        if (w->done.load()) {
            threadWaitForExit(&w->thread);
            threadClose(&w->thread);
            g_workers.erase(g_workers.begin() + i);
            delete w;
        } else {
            i++;
        }
    }
}

bool runDetached(const std::function<void()>& fn) {
    reapFinishedWorkers();

    Worker* w = new Worker();
    w->fn = fn;

    Result rc = threadCreate(&w->thread, workerEntry, w, nullptr, 0x20000, 0x2C, -2);
    if (R_FAILED(rc)) {
        logPrintf("THREAD: threadCreate failed rc=0x%08X -- caller must run inline", (unsigned)rc);
        delete w;
        return false;
    }

    rc = threadStart(&w->thread);
    if (R_FAILED(rc)) {
        logPrintf("THREAD: threadStart failed rc=0x%08X -- caller must run inline", (unsigned)rc);
        threadClose(&w->thread);
        delete w;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_workersMutex);
        g_workers.push_back(w);
    }
    logPrintf("THREAD: started (libnx threadCreate, prio 0x2C, stack 128 KiB)");
    return true;
}

/* ---- the re-confirmation experiment ------------------------------------ */

static std::atomic<int> g_probeRuns{0};

static void probeEntry(void*) { g_probeRuns.fetch_add(1); }

void threadSelfTest() {
    logPrintf("SELFTEST: --- std::thread ---");
    {
        std::atomic<int> ran{0};
        bool threw = false;
        try {
            std::thread t([&ran]() { ran.fetch_add(1); });
            t.join();
            logPrintf("SELFTEST std::thread: no throw; body ran=%d", ran.load());
        } catch (const std::system_error& e) {
            threw = true;
            logPrintf("SELFTEST std::thread: THREW system_error value=%d what='%s'",
                      e.code().value(), e.what());
        } catch (const std::exception& e) {
            threw = true;
            logPrintf("SELFTEST std::thread: THREW std::exception what='%s'", e.what());
        } catch (...) {
            threw = true;
            logPrintf("SELFTEST std::thread: THREW a non-std exception");
        }
        if (threw) {
            logPrintf("SELFTEST std::thread: body ran=%d (0 means no thread ever ran)", ran.load());
        }
    }

    logPrintf("SELFTEST: --- libnx threadCreate ---");
    g_probeRuns.store(0);
    Thread t;
    Result rc = threadCreate(&t, probeEntry, nullptr, nullptr, 0x20000, 0x2C, -2);
    logPrintf("SELFTEST threadCreate: rc=0x%08X", (unsigned)rc);
    if (R_SUCCEEDED(rc)) {
        Result rc2 = threadStart(&t);
        logPrintf("SELFTEST threadStart: rc=0x%08X", (unsigned)rc2);
        if (R_SUCCEEDED(rc2)) {
            /* Bounded wait: 200 x 10 ms = 2 s, after which the answer is "it did
             * not run" rather than a hang. */
            for (int i = 0; i < 200 && g_probeRuns.load() == 0; i++) svcSleepThread(10000000ULL);
            logPrintf("SELFTEST libnx thread body ran=%d (1 means it works)", g_probeRuns.load());
            threadWaitForExit(&t);
        }
        threadClose(&t);
    }
    logPrintf("SELFTEST: --- end ---");
}

/* ---- WAIT FOR IN-FLIGHT HTTP BEFORE THE PROCESS EXITS (build168) ------------------------------------
 *
 * See hg_thread.hpp for the two crash reports this exists for. The shape mirrors the self-test's own
 * bounded wait just above: 10 ms sleeps, a counted loop, and a log line that says what actually
 * happened -- "waited" is a measurement, not an intention.
 *
 * THE COUNT IS READ TWICE, ON PURPOSE. The first read decides whether this is worth a log line at all
 * (the overwhelmingly common case is a clean zero, and a shutdown path that always prints two lines
 * trains people to stop reading them). The last read is the answer to report. */
bool waitForHttpIdle(int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;

    /* ONE LINE EITHER WAY. The common case is a clean zero, and saying so costs one line while leaving
     * the reader unable to tell "the wait found nothing" from "the wait never ran" -- which is exactly
     * the question a log is being read to answer. */
    const int atEntry = hg_http_in_flight();
    if (atEntry <= 0) {
        logPrintf("SHUTDOWN: no HTTP request was in flight at exit -- nothing to wait for [build168]");
        return true;
    }

    logPrintf("SHUTDOWN: %d HTTP request(s) still in flight -- waiting up to %d ms before the process "
              "tears libnx down [build168]",
              atEntry, timeout_ms);

    int waited = 0;
    for (int i = 0; i < timeout_ms / 10; i++) {
        if (hg_http_in_flight() <= 0) break;
        svcSleepThread(10000000ULL);   /* 10 ms, the same unit the self-test above uses */
        waited += 10;
    }

    const int left = hg_http_in_flight();
    logPrintf("SHUTDOWN: waited %d ms for in-flight HTTP (left: %d)%s", waited, left,
              left == 0 ? "" : " -- exiting anyway, and THIS is the one case a teardown race is still"
                               " possible");
    return left == 0;
}

} // namespace hg
