// hg_playback_sink.hpp -- the one callback the player view needs from the screen that owns it.
//
// GPL-3.0-only.
//
// WHY THIS IS ITS OWN HEADER. PlayerView needs to hand its owner a playback position, and PlayerActivity needs
// to implement it -- but player_view.hpp already includes player_activity.hpp (it calls setMenuOverlay on the
// activity directly), so player_activity.hpp cannot include player_view.hpp back. An interface that both can
// include, without either including the other, is the way out of that cycle: the sink knows nothing about
// either class.
//
// WHY AN INTERFACE AT ALL, rather than the view writing history itself: the view knows the episode id and the
// position, and nothing else -- not the series, not its title, not its cover. Those are what a history record
// is made of, and the activity is the object that has them. Handing the view a std::function would work, but
// the sink also documents the LIFETIME rule: the activity clears it in its destructor, so a stray frame can
// never reach a half-destroyed activity (the same rule setMenuOverlayCallback follows).
//
// ASCII-only source file.

#pragma once

#include <string>

namespace hg {

class PlaybackSink {
  public:
    /* DEFINED OUT OF LINE, in hg_playback_sink.cpp. A class whose every virtual is inline has no translation
     * unit that emits its vtable, and the linker then reports the vtable and the typeinfo as undefined -- which
     * is exactly what happened when this was `{}` in the header. */
    virtual ~PlaybackSink();

    /* Called on the main thread, roughly every 15 seconds while an episode plays and once when it starts.
     * `position` and `duration` are seconds; either may be 0 when the player does not know yet. */
    virtual void recordPlayback(const std::string& episodeId, double position, double duration) = 0;

    /* CALLED WHEN AN EPISODE COULD NOT BE STARTED AT ALL, with the reason.
     *
     * WHY THE SCREEN THAT ASKED HAS TO HEAR ABOUT IT (round-26 audit): `startEpisode()` moves the activity's
     * `episodeIndex` BEFORE asking the view to start playing, because the label, the auto-next arithmetic and
     * the ZL/ZR steps all read that index. If the play-plan request then fails -- a dropped connection during an
     * auto-next is the realistic case -- the previous episode keeps playing while the screen calls it "episode
     * N+1", and ZR from there skips one. The view cannot fix that: it does not own the index. So it reports, and
     * the activity puts its index back. */
    virtual void playbackFailed(const std::string& reason) = 0;

    /* CALLED WHEN THE CONTROL BAR APPEARS OR DISAPPEARS (round 31).
     *
     * WHY THE ACTIVITY HAS TO KNOW. The player has TWO progress indicators, and a user photographed both of them
     * at once: the original OVERLAY row ("PLAYING 0:20 / 2:55" plus its thin bar, owned by the activity) and the
     * CONTROL BAR's own progress plus its title line (owned by the view). With the bar up they say the same thing
     * twice, and the report was "the upper progress bar cannot be dismissed, it is not needed".
     *
     * The view cannot fix it alone: the overlay is the activity's, and the view does not know whether the user
     * has hidden it with Y (`overlayShown`). So the view reports, and the activity applies the same rule it
     * already applies when the picture menu opens. */
    virtual void controlBarVisible(bool visible) = 0;
};

} // namespace hg
