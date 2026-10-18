# app/hongguo — the borealis app shell

A Nintendo Switch homebrew client for 红果短剧. Three activities over the
already-verified C data layer in `app/src`.

## Build

```powershell
# 1. Stage borealis into an ASCII scratch dir (once; ~1100 files)
powershell -File scripts\stage-borealis.ps1

# 2. Build borealis as a static library (~100s)
powershell -File scripts\build-borealis.ps1 -Jobs 8

# 3. Build the app NRO
powershell -File scripts\build-hongguo.ps1 -OutNro out\hongguo.nro
```

A smoke test that boots borealis with no network and no mpv, useful for
isolating link/boot problems from the app logic:

```powershell
powershell -File scripts\build-hongguo.ps1 -SmokeTest -OutNro out\hongguo-smoke.nro
```

## Why there is no CMake here

borealis ships a CMake build and the toolchain to drive it exists
(`C:\devkitPro\msys2\usr\bin\cmake.exe`). It cannot run under this sandbox:
every MSYS2 binary aborts at startup with

```
fatal error - couldn't create signal pipe, Win32 error 5
```

MSYS2's runtime creates a named pipe per process for signal emulation and
named-pipe creation is denied here. `make`, `ninja` and `sh` fail identically,
and no native (non-MSYS2) CMake is installed.

`scripts/build-borealis.ps1` therefore reproduces the CMake graph by hand:
every source list, include path and define is transcribed from
`upstream/wiliwili/library/borealis/library/CMakeLists.txt` and
`library/cmake/toolchain.cmake`. This is mechanical transcription, not
guesswork. The one thing dropped is `gen_dksh()` (running `uam` to compile the
nanovg shaders); the prebuilt `.dksh` files may need to be copied into the
NRO's romfs if shader loading fails at runtime — see "unverified" below.

## Files

| File | Role |
|---|---|
| `main.cpp` | Entry point: borealis init, window, `+` quits, pushes Home |
| `hg_client.{hpp,cpp}` | The bridge to the verified C layer. Re-runs the exact call sequence of `app/play-test/main.c`, returning C++ objects instead of printf |
| `home_activity.{hpp,cpp}` | The 36 home cards as a focusable list; A opens detail |
| `detail_activity.{hpp,cpp}` | Episode list for one series; A plays |
| `player_activity.{hpp,cpp}` | Container + input mapping for the player |
| `player_view.{hpp,cpp}` | Owns mpv, the deko3d render context, and the per-frame draw |
| `smoke_main.cpp` | Minimal boot test, built only with `-SmokeTest` |

## Controls

| Button | Home | Detail | Player |
|---|---|---|---|
| A | open detail | play episode | play/pause |
| B | — | back | back |
| + | exit | exit | exit |

B and + are borealis defaults (`setGlobalQuit`); A is registered per activity.

## Audio and video output

`ao` is **unset** on purpose so mpv uses its platform default. The verified
hardware test ran `ao=null`, so sound has never been exercised — leaving it
unset is what makes an audio attempt happen at all. `vo` is not an mpv option
here; video goes through the libmpv render API with
`MPV_RENDER_API_TYPE_DEKO3D`, following
`upstream/wiliwili/wiliwili/source/view/mpv_core.cpp` exactly, including the
`queueSignalFence`/`queueWaitFence` pair around
`mpv_render_context_render` into borealis' current framebuffer. `hwdec=auto`
(wiliwili's Switch default) and `hwdec-current` is drawn on screen so a silent
fallback to software decoding cannot hide.

## Known limitations

- **No cover images.** borealis has no HTTP image loader and no decoder for the
  CDN's WebP/JPEG, so covers would need an HTTP fetch plus a decode path that
  does not exist in this build. The list is text-only and the cover URL is
  printed on the detail page so the field is visibly populated rather than
  silently dropped.
- **Detached worker threads capture raw view pointers.** Backing out of a
  screen before its fetch returns could hand `brls::sync` a destroyed view.
  The window is small (borealis keeps the activity alive through its hide
  animation and the fetches take seconds), but it is not zero. A
  `shared_ptr` token cleared in the destructor is the correct fix.
