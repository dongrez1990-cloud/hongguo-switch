/*
 * hg_thread: worker threads, without std::thread.
 *
 * GPL-3.0-only.
 *
 * WHY NOT std::thread. Measured on real hardware (out/hongguo-diag.nro, log
 * sdmc:/switch/hongguo.log):
 *
 *   HOME: ENTER std::thread(...).detach()
 *   FATAL std::terminate reached (uncaught exception or explicit call)
 *   FATAL exception what() = Function not implemented
 *
 * "Function not implemented" is ENOSYS, and std::thread's constructor throws it
 * as std::system_error. The chain, read out of the built binary rather than
 * guessed at:
 *
 *   std::thread::_M_start_thread   hongguo.elf 0x11bfac0  bl pthread_create
 *                                  hongguo.elf 0x11bfac4  cbnz w0 -> throw_system_error(w0)
 *   pthread_create                 comes from libsysbase.a's pthread.o; its only
 *                                  two ENOSYS exits are taken when the GOT slot
 *                                  for __syscall_thread_create is NULL
 *
 * This devkitA64 has a stub libpthread.a containing a symbol-less dummy.o, and
 * the newlib/libnx pthread glue does not survive this link (verified with nm
 * over the archives and by reading the GOT slot's bytes out of the ELF - the
 * slot holds 0x10c6a70 in the file, so which of the two links failed depends on
 * relocation, not on source). What is NOT in doubt is the observation: on the
 * console, std::thread throws ENOSYS before any thread exists.
 *
 * libnx' own API has no such indirection. threadCreate() takes a Thread struct
 * and goes straight to svcCreateThread; it is what every Switch homebrew uses,
 * and it does not depend on how libstdc++ was configured for this target. So
 * the workers here are built on it.
 *
 * CONSEQUENCE WORTH KNOWING: borealis' own task-loop thread is created with
 * pthread_create (core/thread.cpp:217) and its return value is ignored, so on
 * this platform brls::async() silently never runs a task. Nothing in this app
 * may rely on brls::async(); brls::sync() is unaffected because it is just a
 * mutex-protected queue drained by the main loop, and that mutex works.
 *
 * ASCII-only source file.
 */

#pragma once

#include <functional>

namespace hg {

/* Runs fn() on a new thread. Returns false if the thread could not be created,
 * in which case fn has NOT run and the caller must decide what to do instead
 * (the callers in this app fall back to running the work inline, so a console
 * whose threads are unavailable still shows a home page rather than nothing).
 *
 * The thread is detached: it is never joined, its exit status is never read,
 * and its stack is released by the reaper rather than by the caller. */
bool runDetached(const std::function<void()>& fn);

/* Creates one thread with std::thread and one with libnx threadCreate and
 * records the numeric outcome of each, plus whether the thread body actually
 * ran. Exists so the diagnosis above can be re-confirmed on any future console
 * in one run instead of being taken on trust. Safe to call after borealis has
 * initialised; it sleeps for at most a second. */
void threadSelfTest();

/* ---- WAIT FOR IN-FLIGHT HTTP BEFORE THE PROCESS EXITS (build168) ----------------------------------
 *
 * WHY. On 2026-10-14 the console produced two Atmosphere crash reports (Data Abort / 2168-0002, fault
 * address 0) whose stacks say the same thing: `exit() -> __syscall_exit -> serviceClose ->
 * _bsdDispatchImpl` was tearing libnx's socket service down while another thread was still inside
 * `curl_easy_perform -> Curl_poll -> poll() -> _socketGetFd`. poll() then used a socket the teardown
 * had already destroyed. build167 removed the app's own exit BUTTON -- which is what made the crash
 * easy to reach -- but ANY exit path, the console's own HOME menu included, can still race a fetch
 * that happens to be in flight. This closes that race.
 *
 * WHAT IT DOES. Polls hg_http_in_flight() every 10 ms until the count reaches zero or `timeout_ms`
 * passes, and logs what it waited. THE BOUND MATTERS: a fetch stuck on a dead CDN must not turn
 * "close the app" into "the app never closes", so after the timeout this returns false and the caller
 * exits anyway -- crashing in that one case is better than a program that will not quit.
 *
 * Returns true when nothing was left in flight (including when nothing ever started). */
bool waitForHttpIdle(int timeout_ms);

} // namespace hg
