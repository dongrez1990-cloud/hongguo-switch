/*
 * hg_image: network cover loader for the hongguo client.
 *
 * GPL-3.0-only.
 *
 * WHAT THIS IS. A port of wiliwili's image chain, which is the authority here
 * because it is the working Switch client that shows network cover images:
 *
 *   upstream/wiliwili/wiliwili/include/utils/image_helper.hpp:41   load(url)
 *   upstream/wiliwili/wiliwili/source/utils/image_helper.cpp:216   cpr GET
 *   upstream/wiliwili/wiliwili/source/utils/image_helper.cpp:242   stbi_load_from_memory
 *   upstream/wiliwili/wiliwili/source/utils/image_helper.cpp:260   brls::sync
 *   upstream/wiliwili/wiliwili/source/utils/image_helper.cpp:273   nvgCreateImageRGBA
 *   upstream/wiliwili/wiliwili/source/utils/image_helper.cpp:280   TextureCache::addCache
 *   upstream/wiliwili/wiliwili/source/utils/image_helper.cpp:283   innerSetImage
 *
 * WHAT WAS NOT PORTED, AND WHY. wiliwili's ImageHelper is 123 lines of header
 * and 359 of source, and most of that exists to solve a problem this app does
 * not have:
 *
 *   - cpr + ImageThreadPool + CurlSharedObject: a HTTP library and a thread
 *     pool. This project already has a hardware-verified libcurl wrapper
 *     (app/src/hg_http.h) and a libnx threadCreate wrapper (hg_thread.hpp).
 *     Adding a third-party dependency to re-do what already works was
 *     explicitly out of scope.
 *   - The USE_WEBP branch (image_helper.cpp:234-245, :194-198). libwebp is not
 *     present anywhere under C:\devkitPro\portlibs\switch\lib, so that branch
 *     could not be linked even if it were wanted. It is left out entirely
 *     rather than left in behind an #ifdef that is always false.
 *   - The BOREALIS_USE_GXM DXT-compression branch (image_helper.cpp:247-258,
 *     :19-99). That is PSVita only. This build defines BOREALIS_USE_DEKO3D,
 *     and the deko3d backend takes plain RGBA (dk_renderer.cpp:73-75,
 *     DkImageFormat_RGBA8_Unorm) with no power-of-two alignment at all.
 *   - The request pool, requestMap and the with() recycler. wiliwili needs
 *     them because a RecyclingGrid recycles its cells and reuses ImageHelper
 *     instances. The equivalent here is a per-view token, below.
 *
 * THREADING -- THE ONE PART THAT REALLY MATTERS.
 *
 * Two rules, and breaking either one produces a crash on the console with no
 * message at all, which is the failure mode this project keeps hitting:
 *
 *   1. nvgCreateImageRGBA (and every other nanovg call) runs on the MAIN
 *      THREAD, never on the worker. It ends up in DkRenderer::CreateTexture
 *      (deko3d/dk_renderer.cpp:452), which pushes onto a plain std::list and
 *      calls CMemPool::allocate (:455, :82); none of that is synchronized, and
 *      brls::Image::innerSetImage -> nvgImageSize (views/image.cpp:386) walks
 *      the same list. wiliwili crosses back with brls::sync at
 *      image_helper.cpp:260, and this file does the same.
 *
 *   2. The brls::sync lambda must not touch a brls::Image that has already
 *      been destroyed. A view is destroyed with a plain `delete` while a
 *      fetch for it may still be in flight; the lambda then writes through
 *      freed memory and the console shows a bare error dialog.
 *
 * HOW THE LIFETIME IS MADE SAFE. There is no weak pointer to a brls::View in
 * this borealis and no ptrLock() in the Image API (wiliwili's own
 * image_helper.cpp:143 uses a fork-only View::ptrLock that is NOT present in
 * the vendored header -- upstream/wiliwili/library/borealis/library/include/
 * borealis/views/image.hpp:63-166 has no such method), so ownership is
 * explicit instead:
 *
 *   - One ImageLoader per brls::Image. The CALLER owns it and must keep it
 *     alive exactly as long as the view, and destroy it -- or call cancel() --
 *     BEFORE deleting the view. hg_image.cpp documents the one call site this
 *     requires.
 *   - Every in-flight fetch holds std::shared_ptr<Target>, a token that holds
 *     a raw brls::Image*. The worker's brls::sync lambda captures only that
 *     shared_ptr. ~ImageLoader calls Target::detach(), which takes the token's
 *     own mutex, sets view = nullptr and dead = true; the lambda does the same
 *     take, and installs the texture only if view is still non-null.
 *
 * Deadlock argument, in full, because "it looks fine" is not an argument:
 * detach() and the sync lambda take the SAME mutex, and neither of them ever
 * touches brls::sync while holding it, so the lock is held for a few
 * assignments and nothing else. The lock is therefore a flag plus a small
 * critical section, with no path that waits on the other side.
 *
 * The texture itself is never destroyed by the token: it is owned by the
 * cache. See installAndCache() in hg_image.cpp.
 *
 * CONFIGURATION. Read from sdmc:/switch/hongguo.cfg (see hg_cfg.hpp). The
 * "image." prefix keeps these keys clear of the existing ao / hwdec / vo /
 * renderctx / loadfile / advanced / fetch keys, whose meaning is untouched:
 *
 *   image.enable=1              "0" disables the loader entirely; default 1
 *   image.suffix=               appended verbatim to every URL; default empty
 *   image.cache=200            number of URLs kept; default 200, clamp 1..400
 *
 * The suffix defaults to EMPTY, unlike wiliwili, which always appends a
 * CDN-specific size suffix (image_helper.hpp:84-85: "@672w_378h_1c.jpg").
 * This project's CDN may or may not accept such a suffix, so the default is
 * none: the loader asks for exactly the URL it was given, and a suffix becomes
 * a one-line FTP change instead of a rebuild.
 *
 * ASCII-only source file.
 */

#pragma once

#include <memory>
#include <string>

/* brls::Image is only ever held by pointer here, but the header is included so
 * that a call site needs nothing more than <borealis.hpp> plus this file. */
#include <borealis/views/image.hpp>

namespace hg {

/* The set of live requests for ONE brls::Image.
 *
 * Shared between the ImageLoader, which may be destroyed at any time, and the
 * detached worker plus its brls::sync lambda, which may still be running. The
 * mutex is what makes "the view is gone" visible on the worker side without
 * ever dereferencing a freed pointer: nobody reads `view` except while holding
 * `m`. */
struct ImageTarget {
    brls::Image* view = nullptr;
    bool dead         = false;

    /* Called from ~ImageLoader. After this returns, no brls::sync lambda that
     * already captured this token will touch the view. */
    void detach();

    /* Called from the sync lambda. Returns the view to write to, or nullptr
     * when the loader (and therefore, by contract, the view) is gone. */
    brls::Image* acquireView();
};

/* A network cover loader for one brls::Image.
 *
 * LIFETIME, STATED AS AN OBLIGATION ON THE CALLER:
 *
 *   1. The view must outlive this object, or cancel() must be called first.
 *      The normal arrangement is a member of the same activity that owns the
 *      view:
 *
 *          class CoverCell : public brls::RecyclerCell {
 *              brls::Image* view;
 *              hg::ImageLoader loader;   // declared AFTER view
 *          };
 *
 *      Members are destroyed in reverse declaration order, so with `view`
 *      declared first the loader is destroyed first and the view is still
 *      valid when detach() runs. Declaring them the other way round is the
 *      bug this comment exists to prevent.
 *   2. A RecyclingGrid recycles cells. A cell reused for a different series
 *      must call cancel() (or load() again, which cancels first) before the
 *      new URL is loaded, or the cell can end up showing the previous
 *      series' cover. cancel() is main-thread only, as is load().
 *
 * Not copyable: the token identity is the whole point. */
class ImageLoader {
  public:
    explicit ImageLoader(brls::Image* target);
    ~ImageLoader();

    ImageLoader(const ImageLoader&)            = delete;
    ImageLoader& operator=(const ImageLoader&) = delete;

    /* Fetch `url` off the main thread and install the texture on the main
     * thread. MAIN THREAD ONLY.
     *
     * Safe to call again with a new URL: the previous request is abandoned
     * first. A URL that is already in the texture cache is applied
     * synchronously and performs no network traffic -- this is the same
     * cache-first shortcut as image_helper.cpp:169-175.
     *
     * On any failure the view is left with no texture of its own
     * (innerSetImage(0) is a no-op that logs "Cannot set texture: 0", see
     * views/image.cpp:370-373), so it draws whatever it drew before, and a
     * distinct line naming the failure is written to sdmc:/switch/hongguo.log.
     * Treat "the log is silent" as the bug, not as success. */
    void load(const std::string& url);

    /* MAIN THREAD ONLY. Makes the view untouchable for every in-flight request
     * and makes the current generation unreachable. Does not release the
     * view's current texture and does not delete a cached one. */
    void cancel();

    /* The view this loader installs into. Null after destruction. */
    brls::Image* view() const { return this->m_sync ? this->m_sync->view : nullptr; }

    /* ---- process-wide behaviour, all main-thread only ---- */

    /* Drop every cached texture. Never called automatically today: the deko3d
     * image pool is fixed at startup and exhausted textures fail silently (the
     * mem pool allocator returns null and there is no error path), so this is
     * the manual relief valve rather than a policy. */
    static void clearCache();

    /* Entries currently in the cache, and the configured capacity. */
    static size_t cacheSize();
    static size_t cacheCapacity();

    /* The configured URL size suffix ("" when unset). Appended verbatim. */
    static const std::string& urlSuffix();

    /* True when image.enable is not "0". */
    static bool enabled();

    /* True when `url` is already decoded and resident. Diagnostics only. */
    static bool isCached(const std::string& url);

  private:
    /* Generation counter. Every load() bumps it; a worker whose generation is
     * stale when it gets to the main thread abandons its pixels instead of
     * overwriting a newer cover. This is the role wiliwili's
     * `isCancel` / `cancel()` pair plays (image_helper.hpp:108,
     * image_helper.cpp:327-330), kept as a plain counter so the worker never
     * reads the ImageLoader -- which may be gone -- at all. */
    std::shared_ptr<ImageTarget> m_sync;
    unsigned long m_generation = 0;
};

} // namespace hg
