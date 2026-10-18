// hg_playback_sink.cpp -- see the header. This TU exists for ONE line: the out-of-line destructor, which is
// what gives the interface its vtable. Without it the linker reports `undefined reference to vtable for
// hg::PlaybackSink`, because a class whose every virtual function is defined inline has no TU that emits one.
//
// GPL-3.0-only. ASCII-only source file.

#include "hg_playback_sink.hpp"

namespace hg {

PlaybackSink::~PlaybackSink() = default;

} // namespace hg
