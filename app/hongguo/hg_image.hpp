/*
 * hg_image: fetch a cover image over HTTPS and give it to a brls::Image.
 *
 * GPL-3.0-only.
 *
 * WHY THIS IS SMALL. borealis has no HTTP image loader and no loader registry
 * (views/image.cpp hard-codes its two paths), but it does have the one entry
 * point that matters: Image::setImageFromMem(data, size), which hands ENCODED
 * bytes to nanovg and lets the backend decode them. The deko3d nanovg backend
 * compiles stb_image in (nanovg.c defines STB_IMAGE_IMPLEMENTATION and
 * stbi_load_from_memory is defined in the built libborealis.a), so JPEG and PNG
 * covers decode with no decoder work here at all. That is the whole reason this
 * file is ~120 lines rather than a decoder port.
 *
 * WHY ptrLock. The fetch runs on a worker thread and the texture install happens
 * later on the main thread. Between those two moments the screen can be popped,
 * and a raw view pointer would then be a use-after-free on a console -- a crash
 * with no message, which is the failure mode this project keeps having to
 * eliminate. borealis has a purpose-built answer: View::ptrLock()/ptrUnlock()
 * (core/view.cpp:2554-2567), which Application::mainLoop's deletionPool consults
 * before deleting a view (core/application.cpp:212-224). Holding a lock across
 * the fetch means the view object cannot be freed while the bytes are in flight,
 * and only the MAIN thread ever touches the counter -- the worker touches no view
 * at all.
 *
 * CONFIG. sdmc:/switch/hongguo.cfg, read through hg_cfg:
 *   image.enable=no    turn cover loading off (default: on)
 *   image.suffix=...    appended to every cover URL. wiliwili appends a size
 *                       suffix like "@672w_378h" (utils/image_helper.hpp).
 *                       MEASURED HERE: appending one returns HTTP 400 with a
 *                       JSON error body (scripts/cover-probe.mjs), so this
 *                       default of empty is not caution, it is the only value
 *                       that works. The CDN's own "~tplv-shrink:640:0.image"
 *                       in the URL is what keeps these files small.
 *
 * ASCII-only source file.
 */

#pragma once

#include <borealis.hpp>
#include <atomic>
#include <string>

namespace hg {

/* WHICH IMAGE A VIEW WANTS RIGHT NOW.
 *
 * This exists because of a specific, silent failure. A RecyclerCell is REUSED:
 * the same brls::Image object shows card 3 and then, after scrolling, card 17.
 * A cover fetch started for card 3 can finish after that reuse, and installing it
 * would put card 3's cover on card 17's title -- a screen that looks completely
 * normal and is simply wrong. It is the worst kind of bug this project has had to
 * design against, because nothing crashes and nothing logs.
 *
 * So every load takes a slot. The owner calls want() whenever it points at a new
 * item, and the loader checks stillWants() before installing: a response whose
 * URL is no longer the current one is DROPPED and counted in the log.
 *
 * The slot is a member of the view, and the view is held alive across the fetch
 * by ptrLock, so the slot cannot go away underneath the callback. It is touched
 * only on the main thread. */
class ImageSlot {
  public:
    void want(const std::string& url) { current = url; }
    bool stillWants(const std::string& url) const { return current == url; }
    const std::string& currentUrl() const { return current; }

  private:
    std::string current;
};

/* Starts an asynchronous fetch of `url` and installs it into `target`.
 *
 * Returns false when nothing was started: covers disabled by config, a null or
 * empty target, an empty URL, or a worker thread that could not be created. In
 * every one of those cases a line is written to the breadcrumb log, because a
 * cover that silently never appears is indistinguishable from a broken decoder.
 *
 * `slot` may be null (a one-shot view that is never reused, like the detail
 * page's cover). When it is given, a response that no longer matches it is
 * discarded instead of installed -- see ImageSlot.
 *
 * `owner` IS THE VIEW THAT OWNS `target` AND `slot`, AND PASSING IT IS NOT OPTIONAL WHENEVER THE
 * CALLBACK WILL TOUCH A MEMBER OF IT. A view lock on `target` protects the Image and nothing else,
 * and this callback also dereferences `slot` and `settled` -- which live inside the owning card. A
 * grid rebuild hands the old cards to borealis' free queue while fetches for them are still in
 * flight, so without this the callback wrote into freed memory; the console showed that as an
 * INTERMITTENT crash whose Atmosphère report points at `brls::Threading::performSyncTasks()` with an
 * Instruction Abort, i.e. a queued main-thread task jumping to a wild address after the heap had been
 * corrupted. Pass the card (or the closest view that outlives the slot and the flag). Pass null only
 * when `slot` and `settled` are both null.
 *
 * `maxBytes` is the response cap, and callers should pass the SMALLEST size that
 * fits their source. It exists because a list of visible cells starts a dozen
 * loads at once: at the 2 MiB default that is ~26 MiB of transient buffers, while
 * the list's own covers are ~76 KB (the CDN's "~tplv-shrink:640:0.image" suffix
 * does the shrinking). See the constants at the call sites.
 *
 * Must be called on the main thread (it takes view pointer locks). Safe to call
 * more than once for the same target; the last COMPLETED fetch that still matches
 * the slot wins.
 *
 * `settled` IS A THREE-STATE ANSWER, NOT A FLAG, and it exists because a bug the console found.
 *
 * The boolean return of this function says only that a fetch STARTED. A caller that treats that as
 * "this card has its picture" stops retrying a card whose fetch then failed -- measured on hardware:
 * three URLs were still being refused on every retry pass minutes later, i.e. those cells had given
 * up without ever having a picture.
 *
 * The caller passes an integer which this function zeroes on every attempt and then fills with one of
 * HG_IMG_*. The third state is the important one: without it, a URL that always returns 404 would be
 * re-requested every retry pass for the life of the screen. Pass NULL when the caller does not care. */
enum {
  /* ---- PENDING AND IN_FLIGHT ARE DIFFERENT, AND CONFLATING THEM WAS A RACE (build98) -----------------
   *
   * The user worked out the shape of this themselves: "it is download, load, transport, display -- one of those
   * must be broken; a hundred and forty covers do not crash it and sixteen at a time does not crash it, so maybe
   * it is the LOAD or the TRANSPORT that breaks -- it has not been shown yet, so it is like the load is removed
   * and then the load comes again, and that crashes it."
   *
   * They are right, and the code did exactly what they describe. The retry tick's rule was "ask again unless the
   * state is INSTALLED or FAILED", and PENDING covered BOTH "the loader refused me" and "a fetch is already
   * running". So a cell whose fetch was already in flight was asked again on every pass -- several fetches for one
   * cell, arriving one after another, and each arrival runs the LOAD: nvgCreateImageMem -> innerSetImage, which
   * CREATES a texture and DELETES the previous one. The card's own comment says the delete frees the old texture,
   * but deko3d does not necessarily return that memory in the same frame -- so a new texture could be created
   * while the old one had not been reclaimed yet. That is a load/tran port collision, and it is what a crash
   * during scrolling looks like.
   *
   * So the two cases are separated. PENDING means "no fetch is running, ask me again" and IN_FLIGHT means "one is
   * already running, do not". The retry tick now skips IN_FLIGHT cells, which both removes the collision and cuts
   * the request storm down to what it should always have been: one request per cell at a time. */
  HG_IMG_PENDING = 0,   /* refused, or about to be retried: the caller may ask again */
  HG_IMG_INSTALLED = 1, /* the pixels are in the view: done, never ask again */
  HG_IMG_IN_FLIGHT = 3, /* a fetch IS RUNNING for this view: asking again would start a second one */
  HG_IMG_FAILED = 2     /* a definitive failure (bad status, empty body, or the retry failed too):
                         * asking again would get the same answer, so stop */
};

bool loadImageAsync(brls::Image* target, const std::string& url, ImageSlot* slot = nullptr,
                    size_t maxBytes = 2 * 1024 * 1024, std::atomic<int>* settled = nullptr,
                    brls::View* owner = nullptr);

/* The cap for LIST thumbnails. It is only a FIRST ATTEMPT: loadImageAsync retries
 * once at HG_IMAGE_RETRY_MAX_BYTES when a response does not fit, so this can stay
 * small without any cover being lost.
 *
 * MEASURED over 14 live covers: thirteen are 71-168 KiB, one is 1,117,229 bytes
 * (the CDN's own shrink suffix did not shrink that one). 256 KiB covers the
 * typical case in one request and keeps a dozen visible cells from allocating a
 * megabyte each. */
#define HG_IMAGE_LIST_MAX_BYTES (256u * 1024u)

/* The size a response is retried at when it did not fit. Above the measured
 * worst case (1,091 KiB) with room to spare. */
#define HG_IMAGE_RETRY_MAX_BYTES (1536u * 1024u)

} // namespace hg
