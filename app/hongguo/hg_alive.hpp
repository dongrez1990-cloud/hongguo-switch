// hg_alive.hpp -- a liveness token, so a callback that outlives its screen cannot touch it.
//
// GPL-3.0-only.
//
// WHY THIS EXISTS -- MEASURED, NOT THEORISED (round 31).
//
// Every asynchronous screen in this app does the same three things: start a fetch on a worker, wait for it, and
// post the result back to the main thread with brls::sync so it can touch its own views. Fourteen call sites,
// six files, all of them capturing the ACTIVITY POINTER (`self`/`this`). The fetch takes seconds; the user can
// press B during it.
//
// What happens then was found in the console's own crash reports, and it is not subtle:
//
//     Result 0x4A8 / Data Abort, Fault Address 0x0
//     Stack: brls::Threading::task_loop(void*)          <- a brls::sync task
//     Stack: brls::SwitchVideoContext::recordStaticCommands() / Application::internalMainLoop()
//
// Six reports, all of them dying inside the sync-task loop. And the newest run's log ends right after "UI scale:"
// -- i.e. the app now dies ON STARTUP, because one of those stale callbacks runs against a freed screen before
// the home page is even up.
//
// THE RULE THE FRAMEWORK DOES NOT ENFORCE: `brls::sync` copies a std::function and runs it later. Nothing about
// that knows whether the object the lambda captured still exists, and `View::ptrLock` protects views from
// deletion, NOT activities from being destroyed -- the task queue is not a view tree.
//
// SO EACH SCREEN HOLDS ONE OF THESE, its callbacks capture the token BY VALUE (a shared_ptr, so the token cannot
// die before they do), and they check `dead()` before touching anything. The screen's destructor calls kill().
// The token outlives the screen; the screen does not outlive the token.
//
// THIS IS THE ONE PLACE TO LOOK if a screen ever crashes "for no reason": grep for brls::sync and check that its
// captures include a token.
//
// ASCII-only source file.

#pragma once

#include <atomic>
#include <memory>

namespace hg {

class Alive {
  public:
    bool dead() const { return this->isDead.load(std::memory_order_acquire); }

    void kill() { this->isDead.store(true, std::memory_order_release); }

  private:
    std::atomic<bool> isDead{false};
};

using AliveToken = std::shared_ptr<Alive>;

/* One per screen, made in the constructor. */
inline AliveToken makeAlive() { return std::make_shared<Alive>(); }

/* WHAT A CALLBACK DOES WITH IT, in one line:
 *
 *     const hg::AliveToken alive = self->alive_;                 // pass this into the lambda
 *     hg::runDetached([self, alive, ...]() {
 *         ...fetch...
 *         brls::sync([self, alive, ...]() {
 *             if (alive->dead()) return;                          // <- the whole fix
 *             self->applyResults(...);
 *         });
 *     });
 *
 * A macro rather than a sentence in a comment, because fourteen call sites is fourteen chances to forget:
 * HG_GUARD_ALIVE(alive) expands to that early return. */
#define HG_GUARD_ALIVE(token)                                                                      \
    do {                                                                                           \
        if (!(token) || (token)->dead()) return;                                                   \
    } while (0)

} // namespace hg
