# iONemu: the launcher

**iONemu** is the Qt 6 desktop front end for `ioncore`, modelled on PCSX2: a cover grid of your discs, a game view
that replaces it while a game runs, an Esc pause menu, fullscreen, a camera setup page. GPL-3.0-or-later. The name,
tagline, folder id and version live in `src/app.h`.

## Build and run

```sh
cmake -S frontend -B frontend/build -G Ninja && cmake --build frontend/build     # Qt 6.5+ Widgets, Multimedia, Network, Svg
frontend/build/ionemu                                                        # the launcher
frontend/build/ionemu "path/to/disc.cue"                                     # start a disc directly
frontend/build/ionemu --help
```

The core is built as a subdirectory (`IONCORE_BUILD_TOOLS=OFF`, so `ionrun`/`ionview`/tests stay out of this build;
the top-level `CMakeLists.txt` builds everything). No SDL: Qt Multimedia drives the audio and the webcam.

First run: **Add Disc Folder...** (toolbar or File menu), pick the folder with your `.cue`/`.bin`/`.mdx` images
(subfolders included; `.zip`/`.7z` must be extracted first). Discs are identified by the SHA-1 of the logical image
and cached (`gamelist.json`; unidentified entries are re-checked on every scan). Double-click a cover to start;
right-click for **Start From Entry...** (a directory entry other than the system intro).

**How the games are played:** the console detects movement, not position, and the manuals say to wave your hand in
front of things. With the mouse, moving it or holding the left button is the wave; a still cursor is a still hand.

**In the game view:** mouse = hand (motion while moving or with the left button held) · Space = Play/Pause ·
Backspace = Stop · Right = Next (skips the system intro) · Left = Previous · B = Brightness · O = Open (the game exits
to the system, which returns you to the grid) · Tab (held) = fast-forward (the toolbar's Fast Forward toggles it) ·
Esc = pause menu (Resume / Reset / Fullscreen / Stop) · F11 or Alt+Enter = fullscreen · F = next video filter ·
M = mute · Equals / Minus = volume · F5 / F8 = save / load state (current slot; the Emulation menu has slots 1-5) · D = motion
overlay (blue = where the game is looking, red = movement the console sees) · S = screenshot · W = camera on/off.
All of these are rebindable in Settings > Controls; Esc and Alt+Enter are fixed.

**Settings** (Settings menu, toolbar), seven pages: General (disc folders, start fullscreen, pause on focus loss, the daily update check),
Graphics (filter, integer scaling, hand marker), Audio (gain, mute, buffer), **Camera** (device, live preview with
the white player outline, red motion squares from the console's own detector and blue squares while a game runs;
zoom, sensitivity, mirror; the toolbar's *Camera Setup* opens it directly), **Player** (the stand-in character used
without a camera: Simple or Detailed style, skin, hair and hair style, shirt, wall, floor, window, with a live
preview in which the hand follows the mouse and the console's detector paints its red squares; applies to a running
game), Controls (key bindings), Advanced (motion overlay). Changing the camera in the
dialog starts it live; Cancel goes back to the saved device.

**Covers:** each disc gets a title card rendered from its own intro at scan time (`covers/generated/`). Your own
`<sha1>.png` or `<title>.png` in `covers/` wins, and Settings › **Download Covers...** fetches images from URL
templates (`${sha1}`, `${title}`, `${file}`) or an index JSON that you provide; the launcher ships no artwork.

**Updates:** `UpdateChecker` (`src/updater.cpp`) asks `api.github.com/repos/<kRepo>/releases/latest` once a day,
four seconds after the window is up, and from Help > Check for Updates; the User-Agent carries the version and
nothing else is sent. A newer tag opens `UpdateDialog` (current and new version with dates, download size, the notes
as markdown, Download and Install, Skip This Version, Remind Me Later). The install happens in the dialog with a
progress bar: the release package for the platform is downloaded to the temp folder and swapped in place of the
running copy (Linux: the `$APPIMAGE` file; Windows: the zip is unpacked with Qt's zip reader into `update-staging`
and every file moved over, files in use renamed `*.old` first; macOS: the zip's `.app` replaces the bundle, kept as
`.app.old`), then the launcher restarts and deletes the leftovers at its next start. `kRepo` and `kReleaseDate` live
in `src/app.h`. Development: `--update-json FILE` shows the dialog for a saved reply, `--update-install` presses the
button by itself.

**Damaged discs:** a disc dumped with read errors shows a "!" badge, and starting it offers *Repair... / Play anyway /
Cancel*. Donors are the disc's own duplicate blocks and your other discs; `<name> (repaired)` is written next to the
original, which is then hidden from the library.

**Files** (Linux; Qt's standard locations elsewhere): settings `~/.config/ionemu/ionemu.ini`; data
`~/.local/share/ionemu/` with `saves/<title> [sha1-8].flash` (the console's save chip, written whenever the game
changes it), `states/<title> [sha1-8].slotN.state`, `screenshots/`, `covers/` and `gamelist.json`.

## How it works

- `EmuThread` owns the `ion::Console` and runs the 60 Hz loop. It is paced by the audio sink: `AudioOutput` is a
  push-mode `QAudioSink` opened at the SPU rate (70312 Hz) or at the device's preferred rate with a linear resampler;
  `write()` blocks while the buffer (80 ms by default) is full. Without an audio device the loop paces itself with a
  wall clock. Fast-forward skips audio and steps 8 frames per loop.
- Frames: the thread renders, applies the composite filter (the same `[1 2 1]` smear as `ionview`) and hands a
  `QImage` to `DisplayWidget` through a mutex and a queued `frameReady` signal (coalesced: at most one pending).
- Input: atomics written by the GUI thread (hand position + move serial, mouse button, button mask, fast-forward,
  gain, mute, filter). The hand is "active" for 8 frames after a move or while the button is held.
- The scan runs on a `Scanner` thread using `ion::identify_disc` (about 0.3 s per disc). `.bin` tracks referenced by
  a `.cue` in the same folder are skipped; loose files that are not iON data tracks are counted as skipped; directories of
  extracted programs are accepted too.
- `CameraSource` (GUI thread): `QCamera` → `QVideoSink` → `QImage` (RGB32), choosing the mode nearest 640×480 at
  25 fps or more, uncompressed before MJPEG. Consumers fetch "the newest frame" with their own serial under a mutex:
  the emulation loop hands it to `Console::set_camera_frame` each iteration; the Camera page preview feeds
  `ion::MotionPreview`, the core's own frame detector, so the red squares before a game runs are exactly what a game
  would flag.
- Live options (camera on/off, zoom, sensitivity, mirror, overlay) are atomics read by the loop each frame. The
  overlay (600 polled-word heat values fading over about a second, plus the flagged cells) is copied under the frame
  mutex for the display and the preview.
- Look: the Fusion style with a dark palette, identical on every platform; Lucide icons (ISC, `icons/`) recoloured
  for the palette; the orb logo in `branding/` is the window icon and the About picture.
- Hotkey actions are attached to the window so they work in fullscreen; the pause menu swallows keys; the settings
  dialog is window-modal; console-key bindings beat single-key shortcuts; no signals are emitted from destructors.
- Development flags: `--grab FILE --grab-after MS [--grab-menu | --grab-settings N | --grab-dialog NAME]` save a
  screenshot of the window and exit, `--render-player FILE` and `--render-cover FILE` render the stand-in player and
  a disc's cover; use `QT_QPA_PLATFORM=offscreen` for headless runs.
