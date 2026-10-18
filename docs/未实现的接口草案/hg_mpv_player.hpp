/*
 * hg_mpv_player: the mpv owner, ported from wiliwili's MPVCore.
 *
 * GPL-3.0-only.
 *
 * WHY THIS FILE REPLACES THE HAND-ROLLED INTEGRATION.
 *
 * The previous version of this project owned mpv from inside PlayerView and
 * drove it from a 500 ms polling thread that called mpv_get_property on
 * time-pos / width / height / hwdec-current / pause / current-ao. On the
 * console that produced working audio, a working decoder, "VO: [libmpv]
 * 360x640 yuv420p" -- and no picture at all.
 *
 * The instruction from the user was: stop inventing, do it the way wiliwili
 * does. wiliwili's Switch build works, so its deko3d path is the authority.
 * This file is a faithful port of that path. Every non-obvious decision
 * carries a `mpv_core.cpp:NNN` / `mpv_core.hpp:NNN` reference to the line in
 * <repo>\upstream\wiliwili\wiliwili\ that it came from.
 *
 * WHAT THE POLLING THREAD WAS MISSING, AND WHY THAT MATTERS.
 *
 * MPVCore::on_update (mpv_core.cpp:205-230) is not a convenience: it is the
 * half of the deko3d contract that makes the renderer produce frames. mpv's
 * render.h states the requirement in one sentence:
 *
 *   "Correct functionality is impossible if the update callback is not set,
 *    or not set soon enough after mpv_render_context_create() (the core can
 *    block while waiting for you to call mpv_render_context_update(), and if
 *    the update callback is not correctly set, it will deadlock, or block for
 *    too long)."
 *                     -- C:/devkitPro/portlibs/switch/include/mpv/render.h:261-265
 *
 * and, for the ADVANCED_CONTROL parameter that wiliwili sets at
 * mpv_core.cpp:431:
 *
 *   "You are supposed to call this [mpv_render_context_update] when the update
 *    callback was invoked ... Otherwise, it's a hard requirement that this is
 *    called after each update callback."
 *                     -- render.h:650-658
 *
 * This project had ADVANCED_CONTROL on and never installed the callback, and
 * called mpv_render_context_update() once per draw() and threw the return
 * value away (old player_view.cpp:587). That is the exact contract violation
 * render.h:261-265 describes.
 *
 * THE OTHER SUSPECT, FOUND BY READING libmpv.a ITSELF.
 *
 * `ra_create_dk` (the deko3d ractx_create in libmpv.a, member ra_dk.c.21.o)
 * hardcodes its render pass description:
 *
 *   +0x28 (64-bit) = 0x16ff   <- renderpass state (DkImageFlags_UsageRender
 *                                | _UsagePresent | _Usage2DEngine, which is
 *                                exactly what borealis' own framebuffer layout
 *                                sets at switch_video.cpp:96)
 *   +0x30 (32-bit) = 16384    <- 0x4000 = DkImageFlags_UsageRender (16) << 10
 *   +0x34 (32-bit) = 1024     <- 0x400 = DkImageFlags_UsagePresent (1) << 10
 *   +0x38 (64-bit) = 0x18000  <- load/store op A = 0 << 16
 *   +0x40 (64-bit) = 1024     <- load/store op B = 1 << 10
 *
 * deko3d.h: DkLoadOp_Load = 0, DkStoreOp_Store = 1, DkImageFlags_Shift = 10.
 * So the mpv deko3d backend's render pass is
 * "color: load 0 / store 1, depth: load 0 / store 1" -- a LOAD pass, with no
 * clear. mpv writes only its own letterboxed video rectangle into the target
 * and leaves every other pixel of the framebuffer untouched.
 *
 * That is fine in wiliwili, because there the DRAW ORDER is:
 *
 *   clear            (borealis' static renderCmdlist, switch_video.cpp:146)
 *   ---> present? no, still the same command buffer
 *   mpv render       (mpv_core.cpp:832, queueFlush at :830 first)
 *   nanovg UI        (recorded, submitted at application.cpp:811)
 *   present          (application.cpp:813 -> switch_video.cpp:230)
 *
 * i.e. the clear reaches the GPU BEFORE mpv's load pass. The clear is what
 * makes mpv's write visible; without it mpv draws onto whatever the previous
 * frame left in the framebuffer.
 *
 * If any of that ordering is different in this app, or if the clear is not
 * where it looks like it is, the result is exactly the reported symptom: a
 * screen showing only the theme's clear colour plus the text labels.
 *
 * ASYNC WORK GOES THROUGH hg::runDetached, NEVER brls::sync.
 *
 * wiliwili reaches the main thread from mpv's own callbacks with
 * `brls::sync` (mpv_core.cpp:206, :233). That works for wiliwili because
 * borealis' task_loop thread exists there. THIS PROJECT'S hg_thread.hpp
 * documents, from measurement, that it does not exist here:
 *
 *   "borealis' own task-loop thread is created with pthread_create
 *    (core/thread.cpp:217) and its return value is ignored, so on this
 *    platform brls::async() silently never runs a task."
 *
 * Worse, use-after-free does not need the task loop to be dead -- it needs it
 * to be ALIVE. brls::sync merely QUEUES the lambda (thread.cpp:67-71) and the
 * main loop drains the queue in internalMainLoop (application.cpp:204). If a
 * queued lambda captures `this` and the object is freed before the next
 * iteration of that loop, the lambda runs against freed memory. wiliwili
 * protects itself by subscribing every mpv callback to
 * Application::getExitDoneEvent (mpv_core.cpp:287) so no callback can outlive
 * the core. This port reaches the same guarantee a simpler way: the callbacks
 * do nothing but post to a queue that this object OWNS, the queue is drained
 * explicitly from draw() on the main thread, and the destructor stops the
 * callback before it frees anything.
 *
 * mpv's contract forbids work inside the callbacks anyway:
 *
 *   "never can be called from within the callbacks set with
 *    mpv_set_wakeup_callback() or mpv_render_context_set_update_callback()"
 *                       -- render.h:65-66
 *
 * so a queue plus a main-thread drain is the contract-correct shape, not a
 * workaround.
 *
 * ASCII-only source file.
 */

#pragma once

#include <string>

/* Deliberately NOT including mpv/client.h or mpv/render.h here: the same
 * choice mpv_core.hpp makes is NOT available to us -- wiliwili does include
 * them (mpv_core.hpp:15-16) because its header exposes mpv_handle*. This
 * header is included by player_view.hpp, which home/detail activities may
 * pull in, so the mpv types stay in the .cpp behind opaque declarations. Only
 * `mpv_handle` / `mpv_render_context` are forward-declared, exactly as the mpv
 * headers themselves declare them. */
struct mpv_handle;
struct mpv_render_context;

namespace hg {

/* The plan, in the shape hg_play.h defines it. Forward-declared as a struct
 * tag so the C header does not leak; `#include "hg_play.h"` in the .cpp
 * completes it. */
struct HgPlayPlan;

/* One main-thread action produced by mpv. The callback only ever POSTS one of
 * these; nothing in this enum body runs on mpv's thread. */
enum class HgMpvEvent {
    None,
    /* mpv_render_context_update() was requested -- see onUpdate below. */
    RenderUpdate,
    /* mpv_wait_event produced something worth acting on (see drain()). */
    Wakeup,
    /* The mpv core is telling us a file finished, or that a property that the
     * status line shows has changed. */
    PropertyChanged,
    /* MPV_EVENT_END_FILE with reason == MPV_END_FILE_REASON_ERROR. */
    FileError,
};

/* The single mpv owner.
 *
 * SINGLETON, like MPVCore (mpv_core.hpp:135 `public brls::Singleton<MPVCore>`).
 * wiliwili can afford a singleton because one app plays one thing at a time.
 * Here it also buys something concrete: the mpv callbacks are static functions
 * with a `void*` argument, and having exactly one instance means the pointer
 * argument cannot dangle across a view rebuild. */
class HgMpvPlayer {
  public:
    static HgMpvPlayer& instance();

    /* --- lifecycle ------------------------------------------------------- */

    /* Creates the mpv handle, sets every option, calls mpv_initialize,
     * creates the deko3d render context and installs both callbacks.
     * Port of MPVCore::init (mpv_core.cpp:299-535). `plan` is copied; only
     * `decryption_key` and `header_fields` are consumed. Returns false and
     * leaves the log holding the reason. */
    bool start(const HgPlayPlan& plan);

    /* Creates the mpv core but never opens a stream, for the cfg switch
     * `loadfile=no`. Returns true when the handle exists. */
    bool startWithoutFile(const HgPlayPlan& plan);

    /* Frees the render context, then the core -- mpv's documented order
     * (mpv_core.cpp:551-561). Idempotent. */
    void shutdown();

    bool isValid() const { return renderCtx != nullptr; }
    bool hasCore() const { return mpv != nullptr; }

    /* --- the deko3d path ------------------------------------------------- */

    /* The deko3d draw. Port of the BOREALIS_USE_DEKO3D arm of MPVCore::draw
     * (mpv_core.cpp:823-856). Call once per frame from a view's draw(), on the
     * main thread, before the child views are drawn.
     *
     * `alpha` mirrors wiliwili: the video is only drawn when the overlay is
     * fully opaque (mpv_core.cpp:825 `if (alpha >= 1)`), which is what keeps a
     * stale video frame from lingering after leaving the page. */
    void draw(float alpha = 1.0f);

    /* --- control --------------------------------------------------------- */

    /* Port of MPVCore::resume / pause (mpv_core.cpp:1199-1201), which are
     * `command_async("set", "pause", ...)`. Async, so no libmpv call is made
     * from the main thread while the render thread may be inside
     * mpv_render_context_render. */
    void resume();
    void pause();
    void togglePause();

    /* --- state ----------------------------------------------------------- */

    /* Drain everything mpv queued, on the main thread. Returns the first event
     * worth telling the caller about, or None. See drain() in the .cpp. */
    HgMpvEvent poll();

    /* Human-readable status, in the same shape the previous polling thread
     * produced, so the on-screen line keeps working. Empty until mpv has
     * reported something. */
    std::string statusText();

    /* --- diagnostics ----------------------------------------------------- */

    /* True once mpv's own update callback has been entered at least once.
     * This is the single number that says whether the render callback is
     * live. Logged on entry and on the first few frames. */
    unsigned updateCallbackCount() const;

  private:
    HgMpvPlayer() = default;
    ~HgMpvPlayer() = default;
    HgMpvPlayer(const HgMpvPlayer&) = delete;
    HgMpvPlayer& operator=(const HgMpvPlayer&) = delete;

    /* mpv calls these from its own threads. Static + void* is mpv's signature
     * (`mpv_render_update_fn`, render.h:628), matching
     * MPVCore::on_update / on_wakeup (mpv_core.hpp:508-510). */
    static void onUpdate(void* self);
    static void onWakeup(void* self);

    /* Set by the callbacks, read by poll()/draw() on the main thread.
     * Plain atomics: the callbacks must not touch a mutex that the main
     * thread might hold while blocked inside mpv_render_context_render. */
    struct Shared;

    void* shared = nullptr;

    /* The render callback path. Body of MPVCore::on_update
     * (mpv_core.cpp:205-230), including its deliberate no-op under deko3d. */
    void handleRenderUpdate();

    /* Body of the drain loop in MPVCore::eventMainLoop that this app needs
     * (mpv_core.cpp:905-1154). */
    void handleEvents();

    mpv_handle* mpv = nullptr;
    mpv_render_context* renderCtx = nullptr;

    /* Renders performed so far. draw() logs the first six bracketed
     * (ENTER / fences queued / render returned / complete). */
    int mpvDraws = 0;
};

} // namespace hg
