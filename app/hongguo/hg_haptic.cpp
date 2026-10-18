// hg_haptic.cpp -- see the header for the scope, which is deliberately narrow.
//
// GPL-3.0-only.
//
// ASCII-only source file.

#include "hg_haptic.hpp"

#include <borealis.hpp>
#include <switch.h>   /* svcSleepThread */

#include "hg_cfg.hpp"
#include "hg_log.hpp"
#include "hg_thread.hpp"

namespace hg {

int hapticEnabled(void) {
    /* Cached: this is asked on every pulse, and hg_cfg's own cache already makes a string lookup cheap, but the
     * bool is what the call sites want. A change in the settings screen takes effect on the next pulse because
     * cfgValue re-reads a changed file (see hg_cfg.hpp). */
    static int cached = -1;
    if (cached < 0) cached = hg::cfgIs("app.rumble", "yes") ? 1 : 0;
    return cached;
}

void hapticPulse(unsigned short low, unsigned short high, int ms) {
    if (!hapticEnabled()) return;

    /* THE PLATFORM OWNS THE INPUT MANAGER (application.hpp:83 getPlatform, platform.hpp getInputManager). */
    brls::Platform* platform = brls::Application::getPlatform();
    brls::InputManager* input = platform ? platform->getInputManager() : nullptr;
    if (!input) {
        /* ONCE, not per call: the same rule the other "cannot do it" paths in this app follow. */
        static bool said = false;
        if (!said) {
            said = true;
            hg::logPrintf("HAPTIC: no input manager on this platform -- vibration is unavailable");
        }
        return;
    }

    /* CONTROLLER 0 IS THE CONSOLE'S OWN PAD. SwitchInputManager::sendRumble takes two motor strengths
     * (input.hpp:336), and the pad keeps vibrating until it is told to stop -- so a pulse needs a CLEAR after
     * `ms`, and borealis has no delay API.
     *
     * THE CLEAR RUNS ON A WORKER after a real sleep, not through brls::sync: brls::sync runs on the next frame,
     * which would make every pulse one frame long regardless of `ms` -- a "150 ms" that is really 16 ms is
     * exactly the kind of claim this project refuses to write down. hg::runDetached is the app's own worker
     * (libnx threadCreate; std::thread throws on this toolchain, see hg_thread.hpp).
     *
     * The clear is an IPC call into the input service, so it is safe to make from a worker; if that ever turns
     * out to be false the failure is "the buzz does not stop", not a corrupt state -- and it is bounded by the
     * duration. */
    input->sendRumble(0, low, high);
    const int duration = ms < 30 ? 30 : (ms > 2000 ? 2000 : ms);
    hg::logPrintf("HAPTIC: pulse low=%u high=%u for %d ms", (unsigned)low, (unsigned)high, duration);
    (void)hg::runDetached([input, duration]() {
        svcSleepThread((int64_t)duration * 1000000LL);
        input->sendRumble(0, 0, 0);
    });
}

} // namespace hg
